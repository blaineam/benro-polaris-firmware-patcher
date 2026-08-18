#!/usr/bin/env bash
# ============================================================================
#  Pick and download the astrometry.net index files a given lens actually needs.
#
#    fetch-indexes.sh --focal-min MM --focal-max MM [--sensor-mm 36] --out DIR
#    fetch-indexes.sh --list                 # show the whole 4100 series
#
#  Indexes are upstream DATA, not part of this project: we never ship or mirror
#  them (docs/LICENSE-AUDIT.md rule 6). This downloads them to a directory you
#  choose -- on a Polaris, the microSD.
#
#  Scale rule of thumb: astrometry.net wants index quads roughly 10-100% of the
#  field, so we take every 4100-series scale whose quad range overlaps that band
#  fields your focal range produces. A narrower focal range downloads less.
# ============================================================================
set -euo pipefail

BASE="http://data.astrometry.net/4100"
# index -> quad size range in arcminutes (the published 4100/Tycho-2 series)
SCALES="4107:22:30 4108:30:42 4109:42:60 4110:60:85 4111:85:120 4112:120:170
        4113:170:240 4114:240:340 4115:340:480 4116:480:680 4117:680:1000
        4118:1000:1400 4119:1400:2000"

FMIN=""; FMAX=""; SENSOR=36.0; OUT=""; LIST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --focal-min) FMIN="$2"; shift 2;;
    --focal-max) FMAX="$2"; shift 2;;
    --sensor-mm) SENSOR="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --list) LIST=1; shift;;
    -h|--help) sed -n '2,17p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 1;;
  esac
done

if [ "$LIST" = 1 ]; then
  printf '%-8s %-18s %s\n' index "quad size (arcmin)" "suits a field of about"
  for s in $SCALES; do
    IFS=: read -r n lo hi <<<"$s"
    printf '%-8s %-18s %s\n' "$n" "$lo-$hi" "$(awk -v l="$lo" -v h="$hi" 'BEGIN{printf "%.1f-%.0f deg", l/60.0, h/6.0}')"
  done
  exit 0
fi

[ -n "$FMIN" ] && [ -n "$FMAX" ] && [ -n "$OUT" ] || {
  echo "error: --focal-min, --focal-max and --out are required" >&2; exit 1; }

# field width in degrees for each end of the focal range
FOV_WIDE=$(awk -v s="$SENSOR" -v f="$FMIN" 'BEGIN{printf "%.4f", 2*atan2(s/2,f)*57.29577951}')
FOV_NARROW=$(awk -v s="$SENSOR" -v f="$FMAX" 'BEGIN{printf "%.4f", 2*atan2(s/2,f)*57.29577951}')
echo "[indexes] ${FMIN}mm -> ${FOV_WIDE} deg wide, ${FMAX}mm -> ${FOV_NARROW} deg (sensor ${SENSOR}mm)"

mkdir -p "$OUT"
WANT=""
for s in $SCALES; do
  IFS=: read -r n lo hi <<<"$s"
  # keep this scale if its quad range overlaps 10-100% of any field we might see
  keep=$(awk -v lo="$lo" -v hi="$hi" -v fw="$FOV_WIDE" -v fn="$FOV_NARROW" 'BEGIN{
      band_lo = fn*60*0.20; band_hi = fw*60*0.80;
      print (hi >= band_lo && lo <= band_hi) ? 1 : 0 }')
  [ "$keep" = 1 ] && WANT="$WANT $n"
done
echo "[indexes] needed:$WANT"

total=0
for n in $WANT; do
  f="$OUT/index-$n.fits"
  if [ -f "$f" ]; then echo "  have index-$n.fits"; else
    echo "  fetching index-$n.fits …"
    if command -v curl >/dev/null 2>&1; then curl -sfL -o "$f" "$BASE/index-$n.fits"
    else wget -q -O "$f" "$BASE/index-$n.fits"; fi
  fi
  sz=$(stat -c %s "$f" 2>/dev/null || stat -f %z "$f")
  total=$((total + sz))
done
echo "[indexes] $OUT holds $(awk -v t="$total" 'BEGIN{printf "%.1f MB", t/1048576}') of index files"
