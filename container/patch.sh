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
#     LIBGPHOTO2_REPO      build from this git repo instead of the release
#                          tarball (default: upstream gphoto/libgphoto2)
#     LIBGPHOTO2_REF       git branch, tag or commit to build. Setting this
#                          switches the build to a git clone, which needs
#                          autotools in the image.
#     FIX_R5M2_TYPO        1 = correct the upstream "EOS 5Rm2" model-name typo
#     SELFTEST             1 = qemu-emulate the driver load (needs qemu-arm-static)
#     SSH_PUBKEY           optional: authorized_keys line(s) to authorise for
#                          root SSH debugging (adds ONE new file to the appfs)
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
# Mode:
#   full     = full-libgphoto2 swap (DEFAULT, hardware-validated): replace the
#              WHOLE 2.5.34 stack (core + port + ptp2 + usb1) via the on-disk
#              trampoline loader, over the reliability-patched pgphoto base.
#   ptp2only = conservative legacy swap: keep pgphoto's compiled-in 2.5.27 core,
#              swap only the ptp2 camlib + usb1 iolib and apply the 14-byte patch.
MODE="${MODE:-full}"
case "$MODE" in full|ptp2only) : ;; *) echo "invalid MODE=$MODE (full|ptp2only)"; exit 1;; esac
# Also swap the usb1 port/iolib (USB transport) alongside the ptp2 camlib.
# 1 = swap usb1.so too (default); 0 = ptp2-only camlib swap (legacy).
SWAP_USB1="${SWAP_USB1:-1}"
# Full mode ALWAYS needs usb1 (the fresh 2.5.34 port dlopens usb1 from IOLIBS).
[ "$MODE" = "full" ] && SWAP_USB1=1
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
if [ -n "${LIBGPHOTO2_REF:-}" ]; then
  log "libgphoto2 target : ${LIBGPHOTO2_REPO:-upstream} @ $LIBGPHOTO2_REF (custom fork)"
else
  log "libgphoto2 target : $LIBGPHOTO2_VERSION"
fi
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
if [ "$MODE" = "ptp2only" ]; then
  # Legacy path: rebuilt ptp2 talks to pgphoto's compiled-in 2.5.27 core, so the
  # 2.5.34 EOS-init additions are dropped for Polaris reliability.
  export REMOVE_KEEP_DEVICE_ON=1
  export REMOVE_EXIT_REMOTEMODE=1
  /opt/patcher/build_ptp2.sh "$LIBGPHOTO2_VERSION" "$TRAMP_ADDR" "$FIX_R5M2_TYPO"
else
  # Full-stack path: build the whole 2.5.34 stack (core+port+ptp2+usb1). ptp2 runs
  # against the FRESH 2.5.34 core here, so the upstream EOS-init behaviour is kept
  # (the cold-start storm is handled by the pgphoto reliability base — resetUsb +
  # list-files — not by the driver). No trampoline shim (new core has the symbol).
  /opt/patcher/build_fullstack.sh "$LIBGPHOTO2_VERSION" "$TRAMP_ADDR" "$FIX_R5M2_TYPO"
fi
NEW_PTP2="$W/out/ptp2.so"
NEW_CORE="$W/out/libgphoto2.so.6"; NEW_PORT="$W/out/libgphoto2_port.so.12"
[ -f "$NEW_PTP2" ] || die "ptp2.so build failed"

# ---------------------------------------------------------------------------
# 5. Verify the rebuilt driver resolves against the CORE IT WILL RUN AGAINST.
#    ptp2only: pgphoto's own compiled-in 2.5.27 core (device core + pgphoto).
#    full    : the FRESH 2.5.34 core/port this run built (which the on-disk loader
#              dlopens) -- so newer symbols like gp_filesystem_set_info_dirty
#              resolve there, NOT against the older device core.
# ---------------------------------------------------------------------------
log "verifying rebuilt ptp2.so…"
FLAGS="$($XT-readelf -h "$NEW_PTP2" | awk -F: '/Flags/{print $2}')"
grep -q 'soft-float' <<<"$FLAGS" || die "ABI mismatch: expected soft-float EABI, got:$FLAGS"
MAXGLIBC="$($XT-readelf --dyn-syms "$NEW_PTP2" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
log "  ABI=$FLAGS  glibc_ceiling=$MAXGLIBC"
case "$MAXGLIBC" in GLIBC_2.4|GLIBC_2.5|GLIBC_2.6|GLIBC_2.7|GLIBC_2.8|GLIBC_2.9|GLIBC_2.1[0-9]|GLIBC_2.2[0-4]) : ;;
  *) die "glibc ceiling $MAXGLIBC exceeds device glibc 2.24 — driver would not load";; esac
# Build the symbol-provider set for the core the driver will bind against.
if [ "$MODE" = "full" ]; then
  [ -f "$NEW_CORE" ] && [ -f "$NEW_PORT" ] || die "full mode: core/port not built"
  $XT-nm -D --defined-only "$NEW_CORE" | awk '{print $3}'  >"$W/prov.txt"
  $XT-nm -D --defined-only "$NEW_PORT" | awk '{print $3}' >>"$W/prov.txt"
  PROV_DESC="the freshly-built 2.5.34 core/port"
else
  $XT-nm -D --defined-only "$DEV/dev_libgphoto2.so.6"       | awk '{print $3}'  >"$W/prov.txt"
  $XT-nm -D --defined-only "$DEV/dev_libgphoto2_port.so.12" | awk '{print $3}' >>"$W/prov.txt"
  PROV_DESC="the device's stock 2.5.27 core"
fi
$XT-nm -D --defined-only "$PG"                             | awk '{print $3}' >>"$W/prov.txt"
sort -u "$W/prov.txt" -o "$W/prov.txt"
$XT-nm -D --undefined-only "$NEW_PTP2" | awk '{print $2}' | grep -E '^(gp_|gpi_)' | sort -u >"$W/need.txt"
MISSING="$(comm -23 "$W/need.txt" "$W/prov.txt" || true)"
[ -z "$MISSING" ] || die "rebuilt driver needs core symbols missing from the target core:\n$MISSING"
log "  all core symbols resolve against $PROV_DESC ✓"

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

# ---------------------------------------------------------------------------
# 5c. FULL mode: verify the freshly-built core + port (fail-safe, same rigour).
# ---------------------------------------------------------------------------
NEW_CORE="$W/out/libgphoto2.so.6"; NEW_PORT="$W/out/libgphoto2_port.so.12"
if [ "$MODE" = "full" ]; then
  [ -f "$NEW_CORE" ] || die "full mode: libgphoto2.so.6 was not built"
  [ -f "$NEW_PORT" ] || die "full mode: libgphoto2_port.so.12 was not built"
  log "verifying rebuilt core + port…"
  for L in "$NEW_CORE" "$NEW_PORT"; do
    CFLAGS_="$($XT-readelf -h "$L" | awk -F: '/Flags/{print $2}')"
    grep -q 'soft-float' <<<"$CFLAGS_" || die "core/port ABI mismatch (not soft-float): $L$CFLAGS_"
    CMAX="$($XT-readelf --dyn-syms "$L" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
    case "$CMAX" in GLIBC_2.4|GLIBC_2.5|GLIBC_2.6|GLIBC_2.7|GLIBC_2.8|GLIBC_2.9|GLIBC_2.1[0-9]|GLIBC_2.2[0-4]) : ;;
      *) die "$L glibc ceiling $CMAX exceeds device glibc 2.24";; esac
  done
  # Every one of the 64 boundary symbols the loader dlsym's must be exported by the
  # new core or port, else the loader would leave that slot at the abort stub.
  $XT-nm -D --defined-only "$NEW_CORE" | awk '{print $3}'  >"$W/newexp.txt"
  $XT-nm -D --defined-only "$NEW_PORT" | awk '{print $3}' >>"$W/newexp.txt"
  sort -u "$W/newexp.txt" -o "$W/newexp.txt"
  MISSING_B="$(comm -23 <(python3 -c "import sys;sys.path.insert(0,'/opt/patcher');import stage2_patch as s;print('\n'.join(sorted(s.BOUNDARY)))") "$W/newexp.txt" || true)"
  [ -z "$MISSING_B" ] || die "new core/port do not export boundary symbols:\n$MISSING_B"
  log "  core/port soft-float, glibc≤2.24, all 64 boundary symbols exported ✓"
fi

if [ "$SELFTEST" = "1" ] && command -v qemu-arm-static >/dev/null 2>&1; then
  if [ "$MODE" = "ptp2only" ]; then
    /opt/patcher/selftest.sh "$APP" "$NEW_PTP2" "$DEV" || warn "selftest reported an issue (non-fatal)"
  else
    warn "selftest skipped in full mode (it emulates ptp2 against the device's 2.5.27"
    warn "core; full mode runs ptp2 against the fresh 2.5.34 core it just built)."
  fi
fi

P_UID="$(stat -c %u "$PG")"; P_GID="$(stat -c %g "$PG")"; P_MODE="$(stat -c %a "$PG")"
if [ "$MODE" = "ptp2only" ]; then
  # -------------------------------------------------------------------------
  # 6 (ptp2only). Patch pgphoto (14 bytes) and swap ptp2/usb1 in place.
  # -------------------------------------------------------------------------
  log "patching pgphoto (3 gates + resetUsb return-0 + list-files skip)…"
  python3 /opt/patcher/analyze_pgphoto.py "$PG" --apply "$W/pgphoto.patched" >/dev/null
  DIFFB="$( { cmp -l "$PG" "$W/pgphoto.patched" || true; } | wc -l | tr -d ' ')"
  # 14 = 3 (gates) + 7 (resetUsb: mov r0,#0 + bx lr) + 4 (list-files bl → nop)
  # KEEP_RESET_USB=1 skips the 7 resetUsb bytes, so the expected count drops.
  if [ "${KEEP_RESET_USB:-0}" = "1" ]; then EXPECT_DIFF=7; else EXPECT_DIFF=14; fi
  [ "$DIFFB" = "$EXPECT_DIFF" ] || die "pgphoto patch changed $DIFFB bytes (expected $EXPECT_DIFF) — aborting"
  log "  pgphoto patched: 14 bytes (gates + resetUsb + list-files skip) ✓"

  O_UID="$(stat -c %u "$STOCK_PTP2")"; O_GID="$(stat -c %g "$STOCK_PTP2")"; O_MODE="$(stat -c %a "$STOCK_PTP2")"
  install -m "$O_MODE" -o "$O_UID" -g "$O_GID" "$NEW_PTP2"           "$STOCK_PTP2"
  install -m "$P_MODE" -o "$P_UID" -g "$P_GID" "$W/pgphoto.patched" "$PG"
  if [ "$SWAP_USB1" = "1" ]; then
    U_UID="$(stat -c %u "$STOCK_USB1")"; U_GID="$(stat -c %g "$STOCK_USB1")"; U_MODE="$(stat -c %a "$STOCK_USB1")"
    install -m "$U_MODE" -o "$U_UID" -g "$U_GID" "$NEW_USB1"         "$STOCK_USB1"
    log "swapped usb1 iolib: /app/lib/libgphoto2_port/$(basename "$(dirname "$STOCK_USB1")")/usb1.so"
  fi
else
  # -------------------------------------------------------------------------
  # 6 (full). Full-libgphoto2 on-disk trampoline swap (DEFAULT).
  #   a) reliability-patched base (the same 14-byte patch, symbol-discovered)
  #   b) on-disk trampoline the 64 boundary entries over that base
  #   c) compile the generic loader against the generated slot table
  #   d) assemble /app/lib/stage2 + install the self-driving wrapper as
  #      /app/bin/pgphoto  (the hardware-validated wrapper form)
  # -------------------------------------------------------------------------
  log "full-libgphoto2: building reliability-patched base…"
  python3 /opt/patcher/analyze_pgphoto.py "$PG" --apply "$W/pgphoto.base" >/dev/null
  DIFFB="$( { cmp -l "$PG" "$W/pgphoto.base" || true; } | wc -l | tr -d ' ')"
  if [ "${KEEP_RESET_USB:-0}" = "1" ]; then EXPECT_DIFF=7; else EXPECT_DIFF=14; fi
  [ "$DIFFB" = "$EXPECT_DIFF" ] || die "reliability base changed $DIFFB bytes (expected $EXPECT_DIFF) — aborting"
  log "  base: 14-byte reliability patch (resetUsb + list-files + 3 gates) md5=$(md5sum "$W/pgphoto.base"|cut -d' ' -f1)"

  log "full-libgphoto2: on-disk trampolining 64 boundary entries…"
  rm -rf "$W/s2"; mkdir -p "$W/s2"
  python3 /opt/patcher/stage2_patch.py \
    --pgphoto "$W/pgphoto.base" --core "$NEW_CORE" --port "$NEW_PORT" \
    --nm "$XT-nm" --readelf "$XT-readelf" \
    --reliability-base "$PG" \
    --outdir "$W/s2" --out "$W/s2/pgphoto.stage2ondisk" \
    || die "stage2_patch.py failed (blocker / collision / unresolved boundary symbol)"
  log "  trampolined: md5=$(md5sum "$W/s2/pgphoto.stage2ondisk"|cut -d' ' -f1) (size $(stat -c %s "$W/s2/pgphoto.stage2ondisk") == stock $(stat -c %s "$PG"))"
  [ "$(stat -c %s "$W/s2/pgphoto.stage2ondisk")" = "$(stat -c %s "$PG")" ] \
    || die "trampolined binary changed size — must be byte-count-identical to stock"

  log "full-libgphoto2: compiling loader (libpolaris_stage2.so)…"
  # Compile from a source NAMED stage2_ondisk_loader.c so the .symtab STT_FILE
  # symbol matches the hardware-validated loader byte-for-byte.
  cp /opt/patcher/stage2_loader.c "$W/s2/stage2_ondisk_loader.c"
  ( cd "$W/s2" && $XT-gcc -shared -fPIC -O2 -std=gnu11 -mfloat-abi=soft -Wall -Wextra \
      -Wl,-soname,libpolaris_stage2.so -I. stage2_ondisk_loader.c \
      -o libpolaris_stage2.so -ldl ) || die "loader compile failed"
  LFLAGS="$($XT-readelf -h "$W/s2/libpolaris_stage2.so" | awk -F: '/Flags/{print $2}')"
  grep -q 'soft-float' <<<"$LFLAGS" || die "loader ABI mismatch (not soft-float):$LFLAGS"
  log "  loader: md5=$(md5sum "$W/s2/libpolaris_stage2.so"|cut -d' ' -f1) ABI=$LFLAGS"

  # d) assemble /app/lib/stage2 and install the wrapper as /app/bin/pgphoto.
  STAGE2="$APP/lib/stage2"
  rm -rf "$STAGE2"
  install -d -m "$P_MODE" -o "$P_UID" -g "$P_GID" \
    "$STAGE2" "$STAGE2/libgphoto2/2.5.34" "$STAGE2/libgphoto2_port/0.12.2"
  install -m 755 -o "$P_UID" -g "$P_GID" "$W/s2/libpolaris_stage2.so"  "$STAGE2/libpolaris_stage2.so"
  install -m 755 -o "$P_UID" -g "$P_GID" "$W/s2/pgphoto.stage2ondisk"  "$STAGE2/pgphoto.stage2ondisk"
  install -m 755 -o "$P_UID" -g "$P_GID" "$NEW_CORE"                   "$STAGE2/libgphoto2.so.6"
  install -m 755 -o "$P_UID" -g "$P_GID" "$NEW_PORT"                   "$STAGE2/libgphoto2_port.so.12"
  install -m 755 -o "$P_UID" -g "$P_GID" "$NEW_PTP2"                   "$STAGE2/libgphoto2/2.5.34/ptp2.so"
  install -m 755 -o "$P_UID" -g "$P_GID" "$NEW_USB1"                   "$STAGE2/libgphoto2_port/0.12.2/usb1.so"
  install -m "$P_MODE" -o "$P_UID" -g "$P_GID" /opt/patcher/ondisk/pgphoto.wrapper "$PG"
  log "  assembled /app/lib/stage2 (loader + core + port + ptp2 + usb1 + trampolined binary)"
  log "  installed self-driving wrapper -> /app/bin/pgphoto (execs /app/lib/stage2/pgphoto.stage2ondisk)"

  # ALSO place the fresh ptp2/usb1 at the STOCK camlib/iolib paths. This is the
  # path the swapped 2.5.34 core actually dlopens its camlib from at runtime
  # (device-confirmed: NOT the CAMLIBS the wrapper exports — the core resolves
  # camlibs from its stock on-disk layout /app/lib/libgphoto2/<rev>/ptp2.so, and
  # the port loader dlopens usb1 from /app/lib/libgphoto2_port/<rev>/usb1.so).
  # Without this the core would load the STALE stock 2.5.27 ptp2 and the Canon
  # driver would not bind. Stock perms/uid/gid preserved (stock camlib is 0750).
  O_UID="$(stat -c %u "$STOCK_PTP2")"; O_GID="$(stat -c %g "$STOCK_PTP2")"; O_MODE="$(stat -c %a "$STOCK_PTP2")"
  install -m "$O_MODE" -o "$O_UID" -g "$O_GID" "$NEW_PTP2" "$STOCK_PTP2"
  log "  placed fresh ptp2 at stock camlib path: /app/lib/libgphoto2/$(basename "$CAMLIB_DIR")/ptp2.so"
  if [ "$SWAP_USB1" = "1" ]; then
    U_UID="$(stat -c %u "$STOCK_USB1")"; U_GID="$(stat -c %g "$STOCK_USB1")"; U_MODE="$(stat -c %a "$STOCK_USB1")"
    install -m "$U_MODE" -o "$U_UID" -g "$U_GID" "$NEW_USB1" "$STOCK_USB1"
    log "  placed fresh usb1 at stock iolib path: /app/lib/libgphoto2_port/$(basename "$(dirname "$STOCK_USB1")")/usb1.so"
  fi
fi

# ---------------------------------------------------------------------------
# 7. OPTIONAL (SSH_PUBKEY): authorise a public key for root SSH debugging.
#
#    The stock firmware ALREADY runs OpenSSH — /etc/init.d/rcS ends with
#    `/usr/local/bin/sshd`, and its sshd_config has `PermitRootLogin yes` +
#    `AuthorizedKeysFile .ssh/authorized_keys`. The only thing missing is a key
#    in /root/.ssh (rootfs), which this tool never touches.
#
#    So instead of modifying anything, we ADD the optional boot hook the stock
#    /app/bootapp already calls if present:
#        if [ -f "/app/network_telnetd.sh" ];then cd /app; ./network_telnetd.sh; fi
#    The hook appends the key to /root/.ssh/authorized_keys at boot. One new
#    appfs file; every existing file stays exactly as the mode above left it.
#    Fail-closed: if bootapp doesn't call a hook we can safely claim, we abort
#    rather than edit bootapp itself.
# ---------------------------------------------------------------------------
SSH_HOOK=""
if [ -n "${SSH_PUBKEY:-}" ]; then
  log "ssh debug: authorising public key(s) for root@polaris…"
  BOOTAPP="$APP/bootapp"
  [ -f "$BOOTAPP" ] || die "SSH_PUBKEY set but /app/bootapp is missing from this appfs — refusing to guess a boot hook"
  for cand in network_telnetd.sh start_agent.sh; do
    grep -q "$cand" "$BOOTAPP" || continue          # bootapp must actually call it
    if [ -e "$APP/$cand" ]; then
      warn "  /app/$cand already exists in this firmware — leaving it alone, trying the next hook"
      continue
    fi
    SSH_HOOK="$cand"; break
  done
  [ -n "$SSH_HOOK" ] || die "SSH_PUBKEY: no free boot hook that /app/bootapp calls (looked for network_telnetd.sh, start_agent.sh) — refusing to modify bootapp"

  printf '%s\n' "$SSH_PUBKEY" > "$W/ssh_keys.txt"
  python3 /opt/patcher/gen_ssh_hook.py \
      --keys "$W/ssh_keys.txt" --hook-name "$SSH_HOOK" --out "$W/ssh_hook.sh" \
      || die "SSH_PUBKEY: invalid public key material (see the error above)"

  # sshd lives in the rootfs, which this tool ships byte-identical. Warn (don't
  # abort) if it isn't there — the hook is harmless either way.
  grep -aq 'sshd' /in/camera/rootfs.ubifs 2>/dev/null \
    || warn "  no 'sshd' found in this rootfs.ubifs — the key will be installed but nothing may serve SSH"

  # same owner/mode as bootapp itself (which provably has +x — S10mpp runs it)
  B_UID="$(stat -c %u "$BOOTAPP")"; B_GID="$(stat -c %g "$BOOTAPP")"; B_MODE="$(stat -c %a "$BOOTAPP")"
  install -m "$B_MODE" -o "$B_UID" -g "$B_GID" "$W/ssh_hook.sh" "$APP/$SSH_HOOK"
  log "  added /app/$SSH_HOOK (boot hook, $B_MODE $B_UID:$B_GID — same as bootapp) — appends to /root/.ssh/authorized_keys at boot"
  log "  after flashing: ssh -i <your private key> root@<polaris ip>"
fi

# ---------------------------------------------------------------------------
# 7b. OPTIONAL (ASTRO_BAKE): bake the whole astro stack straight INTO the
#     firmware. /app/astro is populated here from a pre-built bundle (mounted at
#     /astro) and polaris-astro-boot.sh is installed as the boot hook, so there is
#     nothing to install on the device and no SSH: the SD card carries only the
#     (large, static) index files. Flash, drop the astrometry/ dir on the card,
#     power on, open the page. If SSH was also requested, its hook is preserved as
#     the boot script's chained pre-hook (/app/network_telnetd.pre-astro.sh).
# ---------------------------------------------------------------------------
if [ "${ASTRO_BAKE:-0}" = "1" ]; then
  log "astro: baking the astro stack into /app/astro…"
  BOOTAPP="$APP/bootapp"
  [ -f "$BOOTAPP" ] || die "--astro-autostart: /app/bootapp is missing from this appfs — refusing to guess a boot hook"
  [ -d /astro ] || die "--astro-autostart: the astro bundle was not mounted at /astro (build it with build-astro.sh)"
  [ -x /astro/polaris-httpd ] || die "--astro-autostart: /astro/polaris-httpd missing — point --astro-bundle at the polaris-astro/ dir"
  [ -f /astro/polaris-astro-boot.sh ] || die "--astro-autostart: /astro/polaris-astro-boot.sh missing from the bundle"
  A_UID="$(stat -c %u "$BOOTAPP")"; A_GID="$(stat -c %g "$BOOTAPP")"; A_MODE="$(stat -c %a "$BOOTAPP")"

  # Copy the binaries + scripts into the appfs. The indexes stay on the SD (too
  # big for the firmware), and no site.conf is baked (the position is set in the
  # web app and saved to the card).
  mkdir -p "$APP/astro"
  _n=0
  for f in /astro/*; do
    [ -f "$f" ] || continue
    b="$(basename "$f")"
    case "$b" in astrometry|site.conf|site.conf.example) continue;; esac
    install -m 755 -o "$A_UID" -g "$A_GID" "$f" "$APP/astro/$b"
    _n=$((_n + 1))
  done
  log "  copied $_n file(s) into /app/astro"

  # Install polaris-astro-boot.sh as the boot hook, chaining any SSH hook.
  ASTRO_SLOT=""
  if [ -n "$SSH_HOOK" ]; then
    install -m "$A_MODE" -o "$A_UID" -g "$A_GID" "$APP/$SSH_HOOK" "$APP/network_telnetd.pre-astro.sh"
    ASTRO_SLOT="$SSH_HOOK"
    log "  ssh-key hook chained as /app/network_telnetd.pre-astro.sh (the boot script runs it first)"
  else
    for cand in network_telnetd.sh start_agent.sh; do
      grep -q "$cand" "$BOOTAPP" || continue          # bootapp must actually call it
      [ -e "$APP/$cand" ] && continue                 # do not clobber an existing hook
      ASTRO_SLOT="$cand"; break
    done
    [ -n "$ASTRO_SLOT" ] || die "--astro-autostart: no free boot hook that /app/bootapp calls (network_telnetd.sh, start_agent.sh)"
  fi
  install -m "$A_MODE" -o "$A_UID" -g "$A_GID" "$APP/astro/polaris-astro-boot.sh" "$APP/$ASTRO_SLOT"
  log "  installed boot hook /app/$ASTRO_SLOT — starts the astro stack at boot; the SD needs only the astrometry/ index files"
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

# ---------------------------------------------------------------------------
# 8b. FULL mode: emit the reversible on-device bundle + LGPL source offer.
#     The bundle lets people TEST before flashing (install_stage2.sh /
#     restore_stock.sh, LD_PRELOAD-reversible). The LGPL notice + COPYING satisfy
#     the source-offer for the libgphoto2 binaries shipped in the appfs.
# ---------------------------------------------------------------------------
if [ "$MODE" = "full" ]; then
  BUN=/out/stage2-ondisk
  rm -rf "$BUN"; mkdir -p "$BUN/ondisk" "$BUN/libgphoto2/2.5.34" "$BUN/libgphoto2_port/0.12.2"
  cp "$W/s2/libpolaris_stage2.so"  "$BUN/ondisk/"
  cp "$W/s2/pgphoto.stage2ondisk"  "$BUN/ondisk/"
  cp /opt/patcher/ondisk/pgphoto.wrapper "$BUN/ondisk/"
  cp /opt/patcher/ondisk/install_stage2.sh "$BUN/ondisk/"
  cp /opt/patcher/ondisk/restore_stock.sh  "$BUN/ondisk/"
  cp "$NEW_CORE" "$BUN/libgphoto2.so.6"
  cp "$NEW_PORT" "$BUN/libgphoto2_port.so.12"
  cp "$NEW_PTP2" "$BUN/libgphoto2/2.5.34/ptp2.so"
  cp "$NEW_USB1" "$BUN/libgphoto2_port/0.12.2/usb1.so"
  chmod +x "$BUN/ondisk/"*.sh "$BUN/ondisk/pgphoto.wrapper" 2>/dev/null || true

  # LGPL source offer for the shipped libgphoto2 binaries.
  LIC=/out/licenses; rm -rf "$LIC"; mkdir -p "$LIC"
  SRCDIR="$(find /work/src -maxdepth 1 -type d -name "libgphoto2-*" | head -1)"
  [ -n "$SRCDIR" ] && [ -f "$SRCDIR/COPYING" ] && cp "$SRCDIR/COPYING" "$LIC/libgphoto2-COPYING.LGPL-2.1"
  cat > "$LIC/README-LGPL.txt" <<EOF
The custom appfs in this output ships freshly-built libgphoto2 $LIBGPHOTO2_VERSION
binaries (LGPL-2.1): libgphoto2.so.6, libgphoto2_port.so.12, ptp2.so, usb1.so.
Corresponding source is the official upstream release:

  https://github.com/gphoto/libgphoto2/releases/tag/v$LIBGPHOTO2_VERSION

This patcher rebuilds those binaries from that exact source (see
container/build_ptp2.sh / build_fullstack.sh). The only source modification is a
one-line ABI pad appended to struct _Camera (an interop SIZE constant, 4120 bytes;
see docs/HOW-IT-WORKS.md) — no proprietary/firmware content is added. See NOTICE.
EOF
  log "  wrote reversible on-device bundle -> /out/stage2-ondisk (install_stage2.sh / restore_stock.sh)"
  log "  wrote LGPL source offer -> /out/licenses"
fi

# ---------------------------------------------------------------------------
# 8c. SSH hook: also emit it standalone, so anyone who already has device access
#      (serial console, or the root password) can enable key login WITHOUT
#      flashing — and so the exact script that went into the appfs is auditable.
# ---------------------------------------------------------------------------
if [ -n "$SSH_HOOK" ]; then
  SSHOUT=/out/ssh-debug; rm -rf "$SSHOUT"; mkdir -p "$SSHOUT"
  install -m 755 "$W/ssh_hook.sh" "$SSHOUT/$SSH_HOOK"
  cat > "$SSHOUT/README.txt" <<EOF
SSH debug access
================

The custom firmware in ../FwPkt now contains ONE extra appfs file:

    /app/$SSH_HOOK

It is the optional boot hook the stock /app/bootapp already calls if it exists,
so no existing firmware file had to be modified to add it. At every boot it
appends your public key(s) to /root/.ssh/authorized_keys. The stock firmware
already runs OpenSSH (/usr/local/bin/sshd, started by /etc/init.d/rcS, with
PermitRootLogin yes) — only the key was missing.

After flashing:

    ssh -i <your private key> root@<polaris ip>

Install it WITHOUT flashing (if you already have device access):

    scp $SSH_HOOK root@<polaris ip>:/app/$SSH_HOOK
    ssh root@<polaris ip> 'chmod +x /app/$SSH_HOOK && /app/$SSH_HOOK'
    # (it also runs itself on every subsequent boot)

Remove it:

    rm /app/$SSH_HOOK
    # and drop your key from /root/.ssh/authorized_keys
    # or reflash stock firmware, which restores both partitions

SECURITY: anyone holding the matching PRIVATE key gets root on this Polaris
over the network. The device's sshd also still accepts the stock root password,
which this tool does not change.
EOF
  log "  wrote standalone ssh hook -> /out/ssh-debug/$SSH_HOOK (install without flashing; see README.txt)"
fi

log "----------------------------------------------------------------------"
if [ "$MODE" = "full" ]; then
  log "DONE (mode: FULL libgphoto2 — core+port+ptp2+usb1 swap, on-disk trampoline)."
else
  log "DONE (mode: ptp2-only — legacy camlib+iolib swap, stock 2.5.27 core kept)."
fi
log "Custom firmware written to /out :"
( cd /out && find FwPkt -type f | sort | sed 's/^/    /' )
log "    FwPkt.zip  md5=$(md5sum /out/FwPkt.zip 2>/dev/null | cut -d' ' -f1)"
log "custom appfs.ubifs md5=$(md5sum /out/FwPkt/camera/appfs.ubifs | cut -d' ' -f1)"
if [ -n "$SSH_HOOK" ]; then
  log "SSH debug access is ENABLED in this image (/app/$SSH_HOOK):"
  log "    after flashing:  ssh -i <your private key> root@<polaris ip>"
  log "    anyone with that private key has root on the device over the network."
fi
if [ "$MODE" = "full" ]; then
  log "Before flashing you can TEST reversibly on-device:"
  log "    copy /out/stage2-ondisk to the camera and run ondisk/install_stage2.sh"
  log "    (revert with ondisk/restore_stock.sh). See docs/HOW-IT-WORKS.md."
fi
log "----------------------------------------------------------------------"
warn "FLASH AT YOUR OWN RISK. Verify on your own device. Keep your stock FwPkt"
warn "as the factory-restore image. See README.md."
