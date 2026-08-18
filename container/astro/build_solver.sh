#!/bin/bash
# ============================================================================
#  Build `polaris-solve` — the on-device plate solver — from upstream
#  astrometry.net source, GSL-free and with the GPL traps left out.
#
#  Runs inside the patcher container (see docker/Dockerfile).
#
#    build_solver.sh <target> [outdir]
#       target = host   : native build, for testing on the build machine
#                arm    : cross build for the Polaris (soft-float, glibc 2.24)
#
#  What is deliberately NOT compiled in (see docs/LICENSE-AUDIT.md):
#     gsl-an/          GPL v3+   -> replaced by our BSD-3 gslshim/
#     util/md5.c       GPL v2+   -> not in the solve closure at all
#     util/ctmf.c      GPL v3+   -> only reachable via simplexy; we do our own
#     util/simplexy.c            -> star extraction is a separate program
#     catalogs/                  -> brightstars is GPLv2+ Stellarium data
#     cfitsio                    -> only the CLI tools need it
#
#  qfits-an IS compiled in: it is GPL v2+, the solver genuinely needs it to
#  read index files, and that is exactly why `polaris-solve` is its own
#  process and is distributed under GPL v2-or-later.
# ============================================================================
set -euo pipefail

TARGET="${1:-host}"
OUT="${2:-/work/out/astro}"
HERE="$(cd "$(dirname "$0")" && pwd)"
AN_VERSION="${AN_VERSION:-0.98}"
AN_URL="https://codeload.github.com/dstndstn/astrometry.net/tar.gz/refs/tags/${AN_VERSION}"
SRCCACHE="${SRCCACHE:-/work/src}"

log(){ printf '\033[1;36m[solver]\033[0m %s\n' "$*"; }
# the patcher image ships wget, not curl; accept either
fetch(){ # fetch <url> <dest>
  if command -v curl >/dev/null 2>&1; then curl -sfL -o "$2" "$1"
  else wget -q -O "$2" "$1"; fi
}
die(){ printf '\033[1;31m[abort]\033[0m %s\n' "$*" >&2; exit 1; }

case "$TARGET" in
  host) CC=gcc; AR=ar; READELF=readelf; NM=nm; ARCHFLAGS="" ;;
  arm)  CC=arm-linux-gnueabi-gcc; AR=arm-linux-gnueabi-ar
        READELF=arm-linux-gnueabi-readelf; NM=arm-linux-gnueabi-nm
        # softfp: hardware VFP/NEON with the soft-float CALLING convention --
        # matches every binary the device already runs (polestar_app's build
        # attributes say VFPv4 + NEON, no Tag_ABI_VFP_args).
        ARCHFLAGS="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp" ;;
  *) die "target must be 'host' or 'arm'" ;;
esac

mkdir -p "$SRCCACHE" "$OUT"
SRC="$SRCCACHE/astrometry.net-$AN_VERSION"
if [ ! -d "$SRC" ]; then
  log "fetching astrometry.net $AN_VERSION …"
  fetch "$AN_URL" "$SRCCACHE/an-$AN_VERSION.tar.gz" || die "download failed"
  tar xzf "$SRCCACHE/an-$AN_VERSION.tar.gz" -C "$SRCCACHE"
fi
[ -d "$SRC/solver" ] || die "unexpected source layout in $SRC"

# The GPL tree we refuse to build at all. Deleting it (rather than just not
# compiling it) means a stray -I can never pull a GPL header in either.
rm -rf "$SRC/gsl-an" "$SRC/catalogs"

BUILD="$OUT/build-$TARGET"; rm -rf "$BUILD"; mkdir -p "$BUILD/obj" "$BUILD/astrometry"

# astrometry.net's configure COMPILES AND RUNS probe binaries to fill this in,
# which cannot work when cross-compiling. Seed it: every probe here is about
# glibc, and the device's glibc 2.24 answers the same as the container's.
cat > "$BUILD/astrometry/os-features-config.h" <<'EOF'
#define NEED_CANONICALIZE_FILE_NAME 0
#define NEED_DECLARE_QSORT_R 0
#define NEED_QSORT_R 0
#define NEED_SWAP_QSORT_R 0
EOF

CFLAGS="-O2 -std=gnu99 -fPIC $ARCHFLAGS
 -I$SRC/include -I$SRC/include/astrometry -I$SRC/util -I$SRC/libkd -I$SRC/qfits-an
 -I$SRC/solver -I$HERE/gslshim/include -I$BUILD
 -Wno-unused-result -Wno-deprecated-declarations -Wno-unused-but-set-variable
 -DAN_GIT_REVISION=\"polaris-$AN_VERSION\" -DAN_GIT_DATE=\"n/a\" -DAN_GIT_URL=\"n/a\""

# Files that are #included templates, CLI mains, GPL, or simply not needed.
skip_file() {
  case "$(basename "$1" .c)" in
    # GPL, per the licence audit
    md5|ctmf|simplexy) return 0;;
    # #included templates, not translation units
    bl-nl|bl-nl-sort|dfind2|kdtree_internal|kdtree_internal_*) return 0;;
    # upstream bug: tpv.c calls a static from sip.c
    tpv) return 0;;
    # image pipeline / plotting / python / tests we do not build
    test_*|*_test|an-pnmtofits|an-fitstopnm|index_pyutils|pyspherematch|jpl|\
    os-features-test|qsort_reentrant|cairoutils|plotstuff|plot*|dimage|image2xy*|\
    dallpeaks|dcen3x3|dfind|dmedsmooth|dobjects|dselip|dsmooth|downsample-fits|\
    convolve-image|coadd|resample|wcs-resample|wcs-pv2sip|wcs-to-tan|wcsinfo|\
    hpsplit|search-index|get-healpix|histogram2d|sparsematrix|wcs-match) return 0;;
  esac
  return 1
}

log "compiling ($TARGET) …"
ok=0; skipped=0; failed=""
for f in "$SRC"/util/*.c "$SRC"/libkd/*.c "$SRC"/qfits-an/*.c "$HERE"/gslshim/gsl_shim.c \
         "$SRC"/solver/solver.c "$SRC"/solver/quad-utils.c "$SRC"/solver/verify.c \
         "$SRC"/solver/tweak2.c; do
  [ -f "$f" ] || continue
  if skip_file "$f"; then skipped=$((skipped+1)); continue; fi
  b="$(basename "$f" .c)"
  if $CC $CFLAGS -c "$f" -o "$BUILD/obj/$b.o" 2>"$BUILD/obj/$b.err"; then
    ok=$((ok+1))
  else
    failed="$failed $b"
  fi
done
log "  compiled $ok objects (skipped $skipped)"
[ -z "$failed" ] || log "  note: did not compile:$failed"

# Archive, so the link pulls in only what is actually referenced.
$AR rcs "$BUILD/libpolarisastro.a" "$BUILD"/obj/*.o
$CC $CFLAGS -o "$OUT/polaris-solve" "$HERE/polaris-solve.c" \
    "$BUILD/libpolarisastro.a" -lm -lpthread || die "link failed"
log "  linked $OUT/polaris-solve ($(stat -c %s "$OUT/polaris-solve") bytes)"

# ---- fail-closed verification, same rigour as the libgphoto2 build ---------
if [ "$TARGET" = "arm" ]; then
  FLAGS="$($READELF -h "$OUT/polaris-solve" | awk -F: '/Flags/{print $2}')"
  grep -q 'soft-float' <<<"$FLAGS" || die "ABI mismatch: expected soft-float EABI, got:$FLAGS"
  MAXGLIBC="$($READELF --dyn-syms "$OUT/polaris-solve" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1)"
  case "$MAXGLIBC" in
    GLIBC_2.[0-9]|GLIBC_2.1[0-9]|GLIBC_2.2[0-4]) : ;;
    *) die "glibc ceiling $MAXGLIBC exceeds the device's 2.24";;
  esac
  NEEDED="$($READELF -d "$OUT/polaris-solve" | awk -F'[][]' '/\(NEEDED\)/{print $2}' | sort -u | tr '\n' ' ')"
  for n in $NEEDED; do
    case "$n" in
      libm.so.6|libpthread.so.0|libc.so.6) : ;;
      *) die "polaris-solve needs '$n', which is not known to be on the device";;
    esac
  done
  log "  ABI=$FLAGS"
  log "  glibc ceiling $MAXGLIBC, DT_NEEDED: $NEEDED  ✓"
fi

# Assert the licence boundary held: no object from a GPL-excluded file exists.
for gpl in md5 ctmf simplexy; do
  [ -f "$BUILD/obj/$gpl.o" ] && die "licence guard: $gpl.o was compiled — it must not be"
done
[ -d "$SRC/gsl-an" ] && die "licence guard: gsl-an/ still present"
$NM -u "$OUT/polaris-solve" 2>/dev/null | grep -q "gsl_" && die "licence guard: unresolved gsl_ symbols — the shim did not take" || true
log "  licence guards passed (no gsl-an, no md5/ctmf/simplexy, no catalogs)"
# ---- polaris-extract: our own MIT star finder (needs libjpeg) -------------
JPEG_VERSION="${JPEG_VERSION:-9f}"
JPEGSRC="$SRCCACHE/jpeg-$JPEG_VERSION"
if [ ! -d "$JPEGSRC" ]; then
  log "fetching libjpeg $JPEG_VERSION …"
  fetch "https://www.ijg.org/files/jpegsrc.v${JPEG_VERSION}.tar.gz" "$SRCCACHE/jpegsrc.tar.gz" \
    || die "libjpeg download failed"
  tar xzf "$SRCCACHE/jpegsrc.tar.gz" -C "$SRCCACHE"
fi
JPEGBUILD="$BUILD/jpeg"
if [ ! -f "$JPEGBUILD/.libs/libjpeg.a" ]; then
  log "building libjpeg ($TARGET, static) …"
  mkdir -p "$JPEGBUILD"
  ( cd "$JPEGBUILD" && "$JPEGSRC/configure" --enable-static --disable-shared \
      ${TARGET:+$([ "$TARGET" = arm ] && echo --host=arm-linux-gnueabi)} \
      CC="$CC" CFLAGS="-O2 $ARCHFLAGS" >/dev/null 2>&1 && make -j4 >/dev/null 2>&1 ) \
    || die "libjpeg build failed"
fi
$CC -O2 $ARCHFLAGS -I"$JPEGBUILD" -I"$JPEGSRC" -o "$OUT/polaris-extract" \
   "$HERE/polaris-extract.c" "$JPEGBUILD/.libs/libjpeg.a" -lm || die "polaris-extract link failed"
log "  linked $OUT/polaris-extract ($(stat -c %s "$OUT/polaris-extract") bytes, libjpeg static)"
if [ "$TARGET" = "arm" ]; then
  EFLAGS="$($READELF -h "$OUT/polaris-extract" | awk -F: '/Flags/{print $2}')"
  grep -q 'soft-float' <<<"$EFLAGS" || die "polaris-extract ABI mismatch:$EFLAGS"
  ENEEDED="$($READELF -d "$OUT/polaris-extract" | awk -F'[][]' '/\(NEEDED\)/{print $2}' | sort -u | tr '\n' ' ')"
  for n in $ENEEDED; do
    case "$n" in libm.so.6|libc.so.6) : ;; *) die "polaris-extract needs '$n'";; esac
  done
  log "  polaris-extract DT_NEEDED: $ENEEDED  ✓"
fi

# ---- polaris-skysim: the simulated camera (MIT source; links the GPL solver
# libs, so the binary is GPL v2+ like polaris-solve. It is a TEST tool.) ------
$CC $CFLAGS -I"$JPEGBUILD" -I"$JPEGSRC" -o "$OUT/polaris-skysim" \
   "$HERE/polaris-skysim.c" "$BUILD/libpolarisastro.a" "$JPEGBUILD/.libs/libjpeg.a" -lm -lpthread \
   || die "polaris-skysim link failed"
log "  linked $OUT/polaris-skysim ($(stat -c %s "$OUT/polaris-skysim") bytes)"

# ---- polaris-mount: the mount protocol client (MIT, no dependencies) ------
$CC -O2 $ARCHFLAGS -o "$OUT/polaris-mount" "$HERE/polaris-mount.c" -lm \
  || die "polaris-mount link failed"
log "  linked $OUT/polaris-mount ($(stat -c %s "$OUT/polaris-mount") bytes)"
if [ "$TARGET" = "arm" ]; then
  MFLAGS="$($READELF -h "$OUT/polaris-mount" | awk -F: '/Flags/{print $2}')"
  grep -q 'soft-float' <<<"$MFLAGS" || die "polaris-mount ABI mismatch:$MFLAGS"
  MNEEDED="$($READELF -d "$OUT/polaris-mount" | awk -F'[][]' '/\(NEEDED\)/{print $2}' | sort -u | tr '\n' ' ')"
  for n in $MNEEDED; do
    case "$n" in libm.so.6|libc.so.6) : ;; *) die "polaris-mount needs '$n'";; esac
  done
  log "  polaris-mount DT_NEEDED: $MNEEDED  ✓"
fi

log "DONE -> $OUT/polaris-solve  $OUT/polaris-extract  $OUT/polaris-mount"
