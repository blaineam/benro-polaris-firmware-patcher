#!/bin/bash
# Build the FULL libgphoto2 2.5.34 stack for the on-disk trampoline swap
# (the patcher's DEFAULT mode):
#     libgphoto2.so.6   (core, _Camera padded to 4140 for Benro-tail ABI parity)
#     libgphoto2_port.so.12 (port)
#     ptp2.so           (camlib)   -- NO trampoline shim (new core has the symbol)
#     usb1.so           (iolib)
# into /work/out.  This is a thin wrapper over build_ptp2.sh with FULLSTACK=1;
# there is ONE build implementation.
#
#   $1 = libgphoto2 version (e.g. 2.5.34)
#   $2 = trampoline address (hex) -- unused in fullstack (kept for arg parity)
#   $3 = fix R5m2 typo (1/0)
#
# Production, non-traced build: FULLSTACK=1 + POLARIS_DBG=1 (EOS-init non-fatal
# via dbg_patch.py), TRACE left OFF. This is the exact configuration the
# hardware-validated R5 Mark II recipe was built with.
set -euo pipefail
export FULLSTACK=1
# The fresh 2.5.34 core exercises Canon's EOS-init drains as upstream wrote them;
# POLARIS_DBG makes the transient ones non-fatal so the real Canon driver binds
# (see build_ptp2.sh / dbg_patch.py). Always on for the full-stack default.
export POLARIS_DBG=1
exec /opt/patcher/build_ptp2.sh "$@"
