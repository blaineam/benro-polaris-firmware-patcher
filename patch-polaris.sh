#!/usr/bin/env sh
# ============================================================================
#  Benro Polaris libgphoto2 patcher — macOS / Linux launcher
#
#  Everything runs inside Docker, so the only host requirement is Docker.
#
#  Usage:
#     ./patch-polaris.sh --fwpkt <FwPkt-folder-or-zip> [options]
#
#  Options:
#     --fwpkt PATH         stock FwPkt folder (has firmwareInfo) or FwPkt.zip  [required]
#     --libgphoto2 VER     libgphoto2 release to build            (default 2.5.34)
#     --out DIR            output directory                       (default ./out)
#     --ptp2-only          conservative fallback: keep the stock 2.5.27 core, swap
#                          only the ptp2 camlib + usb1 iolib (+ 14-byte pgphoto patch).
#                          DEFAULT (no flag) is the full-libgphoto2 stack swap.
#     --selftest           qemu-emulate the driver load (R5 II registration)
#     --no-fix-typo        do NOT correct the upstream "EOS 5Rm2" model typo
#     --no-usb1            (ptp2-only) do NOT swap the usb1 iolib; patch ptp2 + pgphoto only
#     --ssh-key KEY        enable SSH debugging: authorise this PUBLIC key for
#                          root login (path to a .pub / authorized_keys file, or
#                          the key line itself). Adds one new appfs file; the
#                          stock firmware already runs sshd. Repeatable.
#     --image NAME         docker image tag              (default polaris-patcher)
#
#  READ THE README AND DISCLAIMERS FIRST.  Tested ONLY against FwVer 4.0.0.32
#  with a Canon EOS R5 Mark II.  Flashing firmware is at YOUR OWN RISK.
# ============================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
FWPKT=""; VER="2.5.34"; OUT="$HERE/out"; SELFTEST=0; FIXTYPO=1; SWAPUSB1=1; IMG="polaris-patcher"; MODE="full"
SSHKEY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --fwpkt) FWPKT="$2"; shift 2;;
    --libgphoto2) VER="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --ptp2-only) MODE="ptp2only"; shift;;
    --selftest) SELFTEST=1; shift;;
    --no-fix-typo) FIXTYPO=0; shift;;
    --no-usb1) SWAPUSB1=0; shift;;
    --ssh-key)
      [ -n "${2:-}" ] || { echo "error: --ssh-key needs a value" >&2; exit 1; }
      if [ -f "$2" ]; then K="$(cat "$2")"
      else
        case "$2" in
          ssh-*|ecdsa-*|rsa-sha2-*|sk-*) K="$2";;
          *) echo "error: --ssh-key must be a path to a .pub/authorized_keys file, or a public key line" >&2; exit 1;;
        esac
      fi
      case "$K" in *"PRIVATE KEY"*)
        echo "error: --ssh-key was given a PRIVATE key. Pass the .pub file instead." >&2; exit 1;;
      esac
      SSHKEY="${SSHKEY:+$SSHKEY
}$K"
      shift 2;;
    --image) IMG="$2"; shift 2;;
    -h|--help) sed -n '2,28p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 1;;
  esac
done

[ -n "$FWPKT" ] || { echo "error: --fwpkt is required" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "error: docker not found. Install Docker Desktop / docker." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: docker daemon not running." >&2; exit 1; }

# --- resolve input into a folder that contains firmwareInfo -----------------
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
IN=""
if [ -d "$FWPKT" ] && [ -f "$FWPKT/firmwareInfo" ]; then
  IN="$FWPKT"
elif [ -d "$FWPKT" ] && [ -f "$FWPKT/FwPkt/firmwareInfo" ]; then
  IN="$FWPKT/FwPkt"
elif [ -f "$FWPKT" ]; then                      # a .zip
  echo "[*] extracting $FWPKT …"
  if command -v unzip >/dev/null 2>&1; then unzip -oq "$FWPKT" -d "$STAGE";
  elif command -v python3 >/dev/null 2>&1; then python3 -m zipfile -e "$FWPKT" "$STAGE";
  else echo "error: need 'unzip' or 'python3' to read the zip, or pass an unzipped folder." >&2; exit 1; fi
  if [ -f "$STAGE/firmwareInfo" ]; then IN="$STAGE";
  elif [ -f "$STAGE/FwPkt/firmwareInfo" ]; then IN="$STAGE/FwPkt";
  else echo "error: could not find firmwareInfo inside the zip." >&2; exit 1; fi
else
  echo "error: --fwpkt must be a FwPkt folder (with firmwareInfo) or a FwPkt.zip" >&2; exit 1
fi

mkdir -p "$OUT"
echo "[*] building docker image '$IMG' (first run only)…"
docker build -q -t "$IMG" -f "$HERE/docker/Dockerfile" "$HERE" >/dev/null

echo "[*] running patcher (mode: $MODE)…"
if [ -n "$SSHKEY" ]; then
  echo "[*] SSH debugging: authorising $(printf '%s\n' "$SSHKEY" | grep -cvE '^[[:space:]]*(#|$)') public key(s) for root login"
fi
docker run --rm \
  -e MODE="$MODE" \
  -e LIBGPHOTO2_VERSION="$VER" -e FIX_R5M2_TYPO="$FIXTYPO" -e SELFTEST="$SELFTEST" \
  -e SWAP_USB1="$SWAPUSB1" \
  -e SSH_PUBKEY="$SSHKEY" \
  -v "$IN":/in:ro -v "$OUT":/out \
  "$IMG"

echo
echo "[✓] Output in: $OUT"
echo "    - $OUT/FwPkt/         (unpacked custom firmware)"
echo "    - $OUT/FwPkt.zip      (copy this to your SD card)"
if [ -n "$SSHKEY" ]; then
  echo "    - $OUT/ssh-debug/     (the SSH hook that went in, + how to install/remove it)"
fi
echo "    Keep your STOCK FwPkt as the factory-restore image."
