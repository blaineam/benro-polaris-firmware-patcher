#!/bin/sh
# Dress rehearsal for polaris-autoalign, run ON THE DEVICE.
# Fires the real shutter, solves a substitute frame (daylight), and shows the
# 530 it WOULD send. Dry run: the mount's alignment is never touched.
set -u
rm -f /app/sd/polaris-autoalign.log /tmp/polaris-autoalign.solution /tmp/polaris-autoalign.pending

LAT=35.35199; LON=-119.17208; export LAT LON
FOCAL_MM=380 \
SOLVE_FRAME=/app/sd/astro-bench/259A8092.JPG CAPTURE_WAIT=8 \
  nohup /app/astro/polaris-autoalign.sh > /tmp/aa.out 2>&1 &
AAPID=$!
sleep 2

echo "--- 1. app slews to its alignment star (519 track:0) ---"
# self-locating: a small nudge from wherever the mount actually is, so this
# rehearsal works no matter what state the previous test left it in
POSE=$(/app/astro/polaris-mount --host 127.0.0.1 --port 9090 pose 2>/dev/null)
CUR_ALT=$(echo "$POSE" | sed -n 's/.*"alt_deg":\([-0-9.]*\).*/\1/p')
CUR_AZ=$(echo  "$POSE" | sed -n 's/.*"az_deg":\([-0-9.]*\).*/\1/p')
echo "    currently at alt $CUR_ALT az $CUR_AZ"
TGT_ALT=$(awk -v a="$CUR_ALT" 'BEGIN{a=a+2; if(a<14)a=14; if(a>78)a=78; printf "%.3f", a}')
TGT_AZ=$(awk  -v z="$CUR_AZ"  'BEGIN{printf "%.3f", (z+3)%360}')
/app/astro/polaris-mount --host 127.0.0.1 --port 9090 --lat $LAT --lon $LON \
   --min-alt 12 --max-alt 80 --max-slew 15 goto --alt $TGT_ALT --az $TGT_AZ --no-track 2>&1 | tail -1

echo "--- 2. daemon should now fire the shutter and solve ---"
sleep 50

echo "--- 3. user taps confirm: app sends 530 step:2 ---"
printf '%s\n' "INFO:msg_rcv_from_app_process[69]:rcv msg from App[99]:type:3;code:530;val:step:2;yaw:0.0;pitch:0.0;lat:0;num:1;lng:0;" >> /app/Mlog.txt
sleep 8

kill $AAPID 2>/dev/null
pkill -f polaris-autoalign 2>/dev/null
echo
echo "=== DAEMON LOG ==="
cat /app/sd/polaris-autoalign.log 2>/dev/null
