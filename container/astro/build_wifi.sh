#!/bin/sh
# ===========================================================================
#  build_wifi.sh -- cross-build the userspace the Polaris is missing for
#  concurrent AP + station ("APSTA") mode.
#
#  WHY THIS IS NEEDED: the device brings its access point up from
#  /app/wifi/wifiStarUp.sh -- insmod bcmdhd.ko, ifconfig wlan0 192.168.0.1,
#  hostapd, udhcpd -- and ships NO station-mode tooling at all. There is no
#  wpa_supplicant and no iw. udhcpc is present.
#
#  The hardware can do it. Probed on the device:
#    firmware 43455c0 ... -p2p- ... advertises mchan + dualband
#    bcmdhd.ko contains apsta, rsdb_mode, "virtual iface add", ADD_IF event
#    /sys/class/net/wlan0/phy80211 exists, so cfg80211/nl80211 is live
#
#  So we need, for armv7 softfp / glibc 2.24:
#    libnl-tiny or libnl3  -> nl80211 plumbing
#    iw                    -> create the second virtual interface
#    wpa_supplicant        -> associate the station interface
#
#  Built static where possible so nothing has to be installed into the
#  device's library paths.
#
#     build_wifi.sh <host|arm> [outdir]
# ===========================================================================
set -e
TARGET=${1:-arm}
OUT=${2:-/work/out/wifi}
SRCCACHE="${SRCCACHE:-/work/src}"
BUILD="${BUILD:-/work/build/wifi}"

LIBNL_VERSION=${LIBNL_VERSION:-3.7.0}
WPAS_VERSION=${WPAS_VERSION:-2.10}
IW_VERSION=${IW_VERSION:-5.19}

log(){ printf '\033[1;36m[wifi]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[abort]\033[0m %s\n' "$*" >&2; exit 1; }
fetch(){
  if command -v curl >/dev/null 2>&1; then curl -sfL -o "$2" "$1"
  else wget -q -O "$2" "$1"; fi
}

case "$TARGET" in
  host) CC=gcc; HOSTARG=""; READELF=readelf ;;
  arm)  CC=arm-linux-gnueabi-gcc; HOSTARG="--host=arm-linux-gnueabi"
        READELF=arm-linux-gnueabi-readelf
        # Same ABI as everything else we ship: softfp, matching polestar_app.
        ARCHFLAGS="-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp" ;;
  *) die "target must be 'host' or 'arm'" ;;
esac
export CC
mkdir -p "$SRCCACHE" "$OUT" "$BUILD"

# libnl's configure needs flex and bison to generate its route parsers. The
# patcher image does not carry them (nothing else we build needs a lexer), so
# install them once rather than vendoring a pre-generated tarball.
if ! command -v flex >/dev/null 2>&1 || ! command -v bison >/dev/null 2>&1; then
  log "installing flex/bison (build-time only)"
  (apt-get update -qq && apt-get install -y -qq flex bison >/dev/null 2>&1) \
    || die "could not install flex/bison -- no network, or not a Debian image"
fi

# ---- libnl ---------------------------------------------------------------
NLSRC="$SRCCACHE/libnl-$LIBNL_VERSION"
if [ ! -d "$NLSRC" ]; then
  log "fetching libnl $LIBNL_VERSION"
  V=$(echo "$LIBNL_VERSION" | tr . _)
  fetch "https://github.com/thom311/libnl/releases/download/libnl${V}/libnl-${LIBNL_VERSION}.tar.gz" \
        "$SRCCACHE/libnl.tar.gz" || die "libnl download failed"
  tar xzf "$SRCCACHE/libnl.tar.gz" -C "$SRCCACHE"
fi
NLPREFIX="$BUILD/nlprefix"
if [ ! -f "$NLPREFIX/lib/libnl-3.a" ]; then
  log "building libnl (static)"
  ( cd "$NLSRC" && ./configure $HOSTARG --prefix="$NLPREFIX" \
      --disable-shared --enable-static --disable-cli \
      CFLAGS="-O2 $ARCHFLAGS" >/dev/null 2>&1 \
    && make -j4 >/dev/null 2>&1 && make install >/dev/null 2>&1 ) \
    || die "libnl build failed"
fi
log "  libnl ok"

# ---- iw ------------------------------------------------------------------
IWSRC="$SRCCACHE/iw-$IW_VERSION"
if [ ! -d "$IWSRC" ]; then
  log "fetching iw $IW_VERSION"
  fetch "https://www.kernel.org/pub/software/network/iw/iw-${IW_VERSION}.tar.gz" \
        "$SRCCACHE/iw.tar.gz" || die "iw download failed"
  tar xzf "$SRCCACHE/iw.tar.gz" -C "$SRCCACHE"
fi
log "building iw"
( cd "$IWSRC" && make clean >/dev/null 2>&1 || true
  make CC="$CC" \
       NL3xFOUND=Y NLLIBNAME=libnl-3.0 \
       CFLAGS="-O2 $ARCHFLAGS -I$NLPREFIX/include/libnl3 -DCONFIG_LIBNL30" \
       LDFLAGS="-L$NLPREFIX/lib" \
       LIBS="-lnl-genl-3 -lnl-3 -lm -lpthread" \
       V=1 iw >/dev/null 2>&1 ) || die "iw build failed"
cp "$IWSRC/iw" "$OUT/iw"
log "  iw -> $OUT/iw ($(stat -c %s "$OUT/iw") bytes)"

# ---- wpa_supplicant ------------------------------------------------------
WSRC="$SRCCACHE/wpa_supplicant-$WPAS_VERSION"
if [ ! -d "$WSRC" ]; then
  log "fetching wpa_supplicant $WPAS_VERSION"
  fetch "https://w1.fi/releases/wpa_supplicant-${WPAS_VERSION}.tar.gz" \
        "$SRCCACHE/wpas.tar.gz" || die "wpa_supplicant download failed"
  tar xzf "$SRCCACHE/wpas.tar.gz" -C "$SRCCACHE"
fi
# Internal TLS/crypto: no OpenSSL on the device and none wanted. WPA2-PSK and
# WPA3-SAE both work from the internal implementation.
cat > "$WSRC/wpa_supplicant/.config" <<EOF
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_CRYPTO=internal
# NO SAE (WPA3). It needs elliptic-curve crypto that the internal
# implementation does not provide, and pulling in OpenSSL for it would mean
# shipping a TLS stack to a device that has none. WPA2-PSK reaches every home
# network worth joining here; a WPA3-ONLY network will not associate.
CONFIG_IEEE80211W=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_DEBUG_FILE=y
CFLAGS += -I$NLPREFIX/include/libnl3 $ARCHFLAGS
LIBS += -L$NLPREFIX/lib -lpthread
EOF
log "building wpa_supplicant (internal crypto, nl80211)"
( cd "$WSRC/wpa_supplicant" && make clean >/dev/null 2>&1 || true
  make CC="$CC" -j4 wpa_supplicant wpa_cli wpa_passphrase >/dev/null 2>&1 ) \
  || die "wpa_supplicant build failed"
for b in wpa_supplicant wpa_cli wpa_passphrase; do
  cp "$WSRC/wpa_supplicant/$b" "$OUT/$b"
  log "  $b -> $(stat -c %s "$OUT/$b") bytes"
done

# ---- ABI guard -----------------------------------------------------------
# The device is armv7 SOFT-FLOAT CALLING CONVENTION with glibc 2.24. A binary
# built hard-float loads and then crashes in a way that looks like a driver
# problem, so check rather than hope.
if [ "$TARGET" = arm ]; then
  for b in iw wpa_supplicant wpa_cli wpa_passphrase; do
    if $READELF -A "$OUT/$b" 2>/dev/null | grep -q "Tag_ABI_VFP_args"; then
      die "$b is HARD-FLOAT (Tag_ABI_VFP_args present) -- it will not run on this device"
    fi
    MAXG=$($READELF -V "$OUT/$b" 2>/dev/null | grep -o 'GLIBC_2\.[0-9]*' \
           | sort -t. -k2 -n | tail -1)
    log "  $b: softfp ok, max glibc ${MAXG:-none}"
  done
fi
log "DONE -> $OUT"
