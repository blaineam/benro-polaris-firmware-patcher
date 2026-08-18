#!/usr/bin/env bash
# ============================================================================
#  polaris plate-solve bench  --  times ONLY the matching algorithm
#
#  Feeds astrometry.net a pre-extracted star list (.xyls) instead of an image,
#  so no star-extraction time is included and no GPLv3 `simplexy`/`ctmf` code is
#  involved (see docs/LICENSE-AUDIT.md rule 3). Prints, per field:
#  solved RA/Dec, field size, rotation, wall-clock and peak RSS.
#
#  Usage:
#     bench-solve.sh --demo <astrometry.net demo dir> [--idx <dir>] [--arch amd64|arm/v7]
#
#  --demo   directory holding apod1-5.xyls (astrometry.net's own demo/).
#           NOT shipped here: those images are copyright their APOD authors
#           and the index files are upstream data. Point this at your own
#           checkout of the astrometry.net source tree.
#  --idx    where to cache the wide-field index files; they are downloaded from
#           data.astrometry.net on first run (4116-4119, ~1 MB total).
#
#  This uses Debian's packaged solve-field for a REFERENCE measurement across
#  architectures. It is NOT the binary we would ship: Debian's is hard-float
#  armhf against glibc 2.36, while the Polaris is soft-float against glibc 2.24.
#  Device-accurate numbers need the cross-built binary (phase 2, device leg).
# ============================================================================
set -euo pipefail

DEMO=""; IDX=""; ARCH="amd64"
while [ $# -gt 0 ]; do
  case "$1" in
    --demo) DEMO="$2"; shift 2;;
    --idx)  IDX="$2";  shift 2;;
    --arch) ARCH="$2"; shift 2;;
    -h|--help) sed -n '2,26p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 1;;
  esac
done
[ -n "$DEMO" ] || { echo "error: --demo is required (astrometry.net demo/ dir)" >&2; exit 1; }
[ -f "$DEMO/apod4.xyls" ] || { echo "error: $DEMO does not look like astrometry.net's demo/ (no apod4.xyls)" >&2; exit 1; }
IDX="${IDX:-$DEMO/../.bench-idx}"

mkdir -p "$IDX"
for n in 4116 4117 4118 4119; do
  [ -f "$IDX/index-$n.fits" ] && continue
  echo "[bench] fetching index-$n.fits (Tycho-2 wide-field series)…"
  curl -sfL -o "$IDX/index-$n.fits" "http://data.astrometry.net/4100/index-$n.fits"
done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cp "$DEMO"/apod*.xyls "$WORK/"
cp "$IDX"/index-41*.fits "$WORK/"
{ echo "add_path /w"; for n in 4116 4117 4118 4119; do echo "index index-$n.fits"; done; } > "$WORK/cfg"

cat > "$WORK/run.sh" <<'INNER'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq astrometry.net time >/dev/null 2>&1
echo "arch=$(uname -m)  solve-field $(solve-field --version 2>&1 | head -1)"
run () { # name width height [scale-low scale-high] label
  /usr/bin/time -f "TIMING arch=$(uname -m) field=$1 mode=$6 wall=%e s maxrss=%M KB" \
    solve-field --config /w/cfg --no-plots --overwrite --dir /tmp/out --temp-dir /tmp \
      --x-column X --y-column Y --sort-column FLUX --width "$2" --height "$3" \
      ${4:+--scale-units degwidth --scale-low $4 --scale-high $5} \
      --cpulimit 600 "/w/$1.xyls" 2>&1 \
  | grep -E "Field center: \(RA,Dec\)|Field size|rotation angle|solved with|Did not solve|TIMING"
}
mkdir -p /tmp/out
# field sizes come from astrometry.net's demo/CREDITS
run apod4 719 507 ""  ""  blind     # 34 x 24 deg
run apod4 719 507 20  50  hinted
run apod5 900 675 ""  ""  blind     # 72 x 54 deg
run apod5 900 675 50  90  hinted
INNER

echo "[bench] running on --platform linux/$ARCH …"
docker run --rm --platform "linux/$ARCH" -v "$WORK":/w -w /w debian:12 bash /w/run.sh
