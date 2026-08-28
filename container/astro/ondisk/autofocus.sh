#!/bin/sh
# polaris-autofocus — sweep the camera's focus and settle at the sharpest point.
#
# Astro autofocus, the kind NINA/Ekos run: step the focuser across a range, score
# each frame by how tight the stars are (median HFR of the brightest stars, from
# polaris-extract --focus-metric), and return to the position that scored best.
#
# Focus is driven RELATIVELY (opcode 311) through the ALREADY-REGISTERED control
# link of the running polaris-httpd — POST /api/link/send — the very link the web
# UI uses. No second connection to the head is opened, and the frames come from
# the same-origin /api/snapshot proxy. The head exposes no focuser read-back, so
# "position" is only a relative step count: the sweep runs ONE direction and the
# return approaches the best step from that SAME direction, taking up the
# focuser's backlash consistently so best focus is reproduced, not missed by slop.
#
# Env in:  HTTP_PORT (required)  ASTRO (dir holding polaris-extract)
#   tunables: AF_STEPS AF_ADJ AF_SETTLE AF_BACKLASH  DOWNSAMPLE
#   files:    AF_JSON (progress, polled by the web UI)
#   test seams: AF_CURL (default curl)  AF_EXTRACT (default $ASTRO/polaris-extract)
#
# Exit 0 on a completed sweep, non-zero if it could not measure a single frame.

PORT="${HTTP_PORT:-8080}"
ASTRO="${ASTRO:-/app/sd/polaris-astro}"
STEPS="${AF_STEPS:-12}"
ADJ="${AF_ADJ:-1}"
SETTLE="${AF_SETTLE:-0.8}"
BACKLASH="${AF_BACKLASH:-3}"
DS="${DOWNSAMPLE:-1}"
JSON="${AF_JSON:-/tmp/polaris-af.json}"
FRAME="${AF_FRAME:-/tmp/polaris-af.jpg}"
CURL="${AF_CURL:-curl}"
EXTRACT="${AF_EXTRACT:-$ASTRO/polaris-extract}"

focus() {   # $1 = signed adj; nudge the focuser through the http link
  "$CURL" -s -m 5 -o /dev/null -X POST "http://127.0.0.1:$PORT/api/link/send" \
    --data "cmd=311&args=mode:1;adj:$1;" 2>/dev/null
}
snap() {    # grab one same-origin frame
  "$CURL" -s -m 8 -o "$FRAME" "http://127.0.0.1:$PORT/api/snapshot" 2>/dev/null
}
score_of() { echo "$1" | sed -n 's/.*score=\([0-9][0-9.]*\).*/\1/p'; }
hfr_of()   { echo "$1" | sed -n 's/.*hfr=\(-*[0-9][0-9.]*\).*/\1/p'; }

# --- take up backlash at the near end, then sweep outward ---
i=0; while [ "$i" -lt "$BACKLASH" ]; do focus "-$ADJ"; i=$((i+1)); done
[ "$SETTLE" != "0" ] && sleep "$SETTLE"

best="-1"; beststep=0; besthfr="null"; samples=""; measured=0; step=0
while [ "$step" -lt "$STEPS" ]; do
  focus "$ADJ"
  [ "$SETTLE" != "0" ] && sleep "$SETTLE"
  line=""
  if snap; then line=$("$EXTRACT" --jpeg "$FRAME" --downsample "$DS" --sigma 4 --focus-metric 2>/dev/null); fi
  sc=$(score_of "$line"); hf=$(hfr_of "$line")
  [ -z "$sc" ] && sc=0
  [ -z "$hf" ] && hf="null"
  [ "$sc" != "0" ] && measured=$((measured+1))
  samples="$samples{\"step\":$step,\"score\":$sc,\"hfr\":$hf},"
  if awk "BEGIN{exit !($sc > $best)}"; then best="$sc"; beststep="$step"; besthfr="$hf"; fi
  step=$((step+1))
  printf '{"state":"running","step":%d,"steps":%d,"best":{"step":%d,"score":%s,"hfr":%s},"samples":[%s]}\n' \
    "$step" "$STEPS" "$beststep" "$best" "$besthfr" "${samples%,}" > "$JSON"
done

# --- return to best focus, approaching from the SWEEP direction (take up backlash) ---
# After the loop the focuser sits at the far end (STEPS forward nudges from start).
# Best focus is at forward-position (beststep+1). Move near past it by BACKLASH,
# then forward BACKLASH, so the final motion is outward — matching how the winning
# frame was measured. Net displacement = -(STEPS-1-beststep).
back=$(( (STEPS - 1 - beststep) + BACKLASH ))
i=0; while [ "$i" -lt "$back" ]; do focus "-$ADJ"; i=$((i+1)); done
[ "$SETTLE" != "0" ] && sleep "$SETTLE"
i=0; while [ "$i" -lt "$BACKLASH" ]; do focus "$ADJ"; i=$((i+1)); done

if [ "$measured" -eq 0 ]; then
  printf '{"state":"failed","step":%d,"steps":%d,"error":"no live-view frame could be scored — turn preview on","best":{"step":0,"score":0,"hfr":null},"samples":[%s]}\n' \
    "$STEPS" "$STEPS" "${samples%,}" > "$JSON"
  exit 1
fi
printf '{"state":"done","step":%d,"steps":%d,"best":{"step":%d,"score":%s,"hfr":%s},"samples":[%s]}\n' \
  "$STEPS" "$STEPS" "$beststep" "$best" "$besthfr" "${samples%,}" > "$JSON"
exit 0
