#!/bin/bash
# Optional offline sanity test: under qemu-arm user emulation, load the rebuilt
# ptp2.so against the DEVICE'S OWN 2.5.27 core and confirm the camera model table
# registers (incl. the Canon R5 Mark II, USB 04a9:3314).
#
# This proves ABI compatibility + driver registration. It does NOT and cannot
# test real USB capture (no camera in the container).
#   $1 = extracted appfs tree (…/ubifs)
#   $2 = rebuilt ptp2.so
#   $3 = /work/devlibs (device libexif/libltdl + dev_libgphoto2*)
set -euo pipefail
APP="$1"; NEWPTP="$2"; DEV="$3"
XT=arm-linux-gnueabi
SRC="$(find /work/src -maxdepth 1 -name 'libgphoto2-*' -type d | head -1)"
T=/work/selftest; rm -rf "$T"; mkdir -p "$T/applib/camlibs"

cp "$DEV/dev_libgphoto2.so.6"       "$T/applib/libgphoto2.so.6";      ln -sf libgphoto2.so.6 "$T/applib/libgphoto2.so"
cp "$DEV/dev_libgphoto2_port.so.12" "$T/applib/libgphoto2_port.so.12";ln -sf libgphoto2_port.so.12 "$T/applib/libgphoto2_port.so"
for l in libexif.so.12 libltdl.so.7; do
  s="$(find "$APP/lib" -name "${l}*" ! -name '*.la' | sort | tail -1)"; [ -n "$s" ] && cp -a "$s" "$T/applib/$l"
done
cp "$NEWPTP" "$T/applib/camlibs/ptp2.so"

cat > "$T/h.c" <<'EOF'
#include <stdio.h>
#include <gphoto2/gphoto2-camera.h>
#include <gphoto2/gphoto2-abilities-list.h>
int main(void){
  GPContext*ctx=gp_context_new(); CameraAbilitiesList*al;
  if(gp_abilities_list_new(&al)<0){printf("list_new fail\n");return 2;}
  int r=gp_abilities_list_load(al,ctx); int n=gp_abilities_list_count(al);
  printf("driver load r=%d models=%d\n",r,n);
  int idx=gp_abilities_list_lookup_model(al,"Canon EOS R5m2");
  if(idx<0){printf("R5m2: NOT FOUND (%d)\n",idx);return 3;}
  CameraAbilities a; gp_abilities_list_get_abilities(al,idx,&a);
  printf("R5m2 OK: usb=%04x:%04x ops=0x%x port=0x%x\n",a.usb_vendor,a.usb_product,a.operations,a.port);
  return 0;
}
EOF
$XT-gcc "$T/h.c" -I"$SRC" -I"$SRC/libgphoto2_port" -I/opt/lg/include \
   -o "$T/h" -L"$T/applib" -lgphoto2 -Wl,-rpath-link,"$T/applib"

echo "[selftest] emulating driver load…"
QEMU_LD_PREFIX=/usr/$XT LD_LIBRARY_PATH="$T/applib" CAMLIBS="$T/applib/camlibs" \
  qemu-arm-static "$T/h"
