#!/bin/sh
# Does the confirm-detection + injection fire? Solution is pre-seeded so this
# tests only the 530 detection path.
set -u
rm -f /app/sd/polaris-autoalign.log /tmp/polaris-autoalign.*
LAT=35.35199; LON=-119.17208; export LAT LON
FRAME=/app/sd/astro-bench/259A8092.JPG nohup /app/astro/polaris-autoalign.sh >/tmp/aa.out 2>&1 &
sleep 2
echo "43.665603 145.996013" > /tmp/polaris-autoalign.solution   # pretend a solve landed
echo "--- appending the app's confirm (530 step:2) ---"
printf '%s\n' "INFO:rcv msg from App[99]:type:3;code:530;val:step:2;yaw:0.0;pitch:0.0;lat:0;num:1;lng:0;" >> /app/Mlog.txt
sleep 6
pkill -f polaris-autoalign 2>/dev/null
echo "=== LOG ==="; cat /app/sd/polaris-autoalign.log 2>/dev/null
echo "=== stderr ==="; tail -5 /tmp/aa.out 2>/dev/null
