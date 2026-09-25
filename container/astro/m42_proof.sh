#!/usr/bin/env bash
# Offline proof using Blaine's real M42 frame:
#   solved RA/Dec (measured on the Polaris) + his GPS + the EXIF capture time
#   -> where the optics really pointed -> inject via 530 -> is the mount fixed?
set -u
LAT=35.35199; LON=-119.17208; UTC="2025-01-18T06:50:44"
SOLVED_RA=83.800859; SOLVED_DEC=-5.161810      # measured on device from 259A8092.JPG
COMPASS_ERR=${1:-8.0}                          # a realistic phone-compass error
PORT=9590; SPORT=9591
M=(./polaris-mount --host 127.0.0.1 --port $PORT --lat $LAT --lon $LON --utc "$UTC")

truth=$(./polaris-mount --lat $LAT --lon $LON --utc "$UTC" radec2altaz --ra $SOLVED_RA --dec $SOLVED_DEC)
T_ALT=$(echo "$truth" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
T_AZ=$(echo  "$truth" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
echo "the frame was taken pointing at: alt $T_ALT  az $T_AZ"
echo "the mount's compass is wrong by:  $COMPASS_ERR deg"

python3 polaris-sim.py --port $PORT --status-port $SPORT --lat $LAT --lon $LON \
        --az-error "$COMPASS_ERR" --jitter 0 --seed 3 >/dev/null 2>&1 &
SIM=$!; trap 'kill $SIM 2>/dev/null' EXIT
sleep 2

# put the OPTICS physically on M42: believed = true - error
BEL_AZ=$(python3 -c "print((($T_AZ) - ($COMPASS_ERR)) % 360)")
"${M[@]}" --max-slew 360 --min-alt 5 goto --alt "$T_ALT" --az "$BEL_AZ" --no-track >/dev/null 2>&1

st() { python3 -c "
import socket,json
s=socket.create_connection(('127.0.0.1',$SPORT),timeout=5); d=json.loads(s.recv(65536)); s.close()
print(f\"true alt {d['true_alt_deg']:.4f} az {d['true_az_deg']:.4f} | mount BELIEVES az {d['reported_az_deg']:.4f} | frame error {d['az_error_deg']:+.4f}\")"; }

echo; echo "BEFORE the plate solve:"; st
echo "  -> the mount thinks it is $COMPASS_ERR deg from where it actually is."

echo; echo "inject the plate-solved truth via 530 (the app's own alignment command):"
"${M[@]}" star-align --alt "$T_ALT" --az "$T_AZ" 2>/dev/null | tail -1

echo; echo "AFTER:"; st

echo; echo "does it now POINT correctly? goto Betelgeuse (RA 88.793 Dec 7.407):"
want=$("${M[@]}" radec2altaz --ra 88.793 --dec 7.407)
W_ALT=$(echo "$want" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
W_AZ=$(echo  "$want" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
echo "  it should end up at alt $W_ALT az $W_AZ"
"${M[@]}" --max-slew 360 goto-radec --ra 88.793 --dec 7.407 --no-track >/dev/null 2>&1
python3 -c "
import socket,json,math
s=socket.create_connection(('127.0.0.1',$SPORT),timeout=5); d=json.loads(s.recv(65536)); s.close()
a1,z1=math.radians(d['true_alt_deg']),math.radians(d['true_az_deg'])
a2,z2=math.radians($W_ALT),math.radians($W_AZ)
sep=math.degrees(math.acos(max(-1,min(1,math.sin(a1)*math.sin(a2)+math.cos(a1)*math.cos(a2)*math.cos(z1-z2)))))
print(f\"  it actually ended up at alt {d['true_alt_deg']:.4f} az {d['true_az_deg']:.4f}\")
print(f\"  POINTING ERROR: {sep*60:.2f} arcmin   (uncorrected it would be ~{$COMPASS_ERR*60:.0f} arcmin)\")"
