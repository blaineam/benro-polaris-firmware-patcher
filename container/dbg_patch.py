#!/usr/bin/env python3
# ===========================================================================
#  POLARIS_DBG production patch — EOS-init NON-FATAL (no tracing).
#
#  This is the SHIPPABLE half of trace_patch.py: it reproduces Benro's
#  POLARIS_DBG behaviour (the two strings "check_eos_events non-fatal=0x%x"
#  and "keep_device_on non-fatal=%d") that lets Canon EOS camera_init COMPLETE
#  on the R5 Mark II instead of aborting and falling back to the generic
#  "USB PTP Class Camera" driver — WITHOUT any of the throwaway
#  fprintf(stderr,"POLARIS_TRACE: ...") diagnostic instrumentation.
#
#  It applies ONLY the behaviour-changing swallows (identical anchors + counts
#  to trace_patch.py, minus the fprintf/fflush):
#     camlibs/ptp2/config.c   C_PTP(ptp_check_eos_events(params));  x13  -> swallow
#     camlibs/ptp2/library.c  C_PTP(ptp_check_eos_events(params));  x1   -> swallow
#     camlibs/ptp2/library.c  CR(camera_keep_device_on(camera));    x3   -> swallow
#
#  The message-carrying `C_PTP_REP_MSG(ptp_check_eos_events(...), ...)` capture-
#  path drains do NOT match `C_PTP (...)` and stay FATAL (upstream semantics),
#  exactly as in the device-proven traced build. setremotemode/seteventmode are
#  left as pristine upstream C_PTP (the traced build only *traced* them; it kept
#  them fatal, so leaving them untouched is behaviourally identical).
#
#  Adds NO exported symbols and NO new DT_NEEDED (uint16_t/int are language
#  types; no libc call is introduced) so the trampoline boundary set, CAMLIBS
#  loading, and DT_NEEDED resolve identically to the clean build.
#
#  Invoked from build_ptp2.sh when POLARIS_DBG=1. Idempotent (re-run safe).
#  Each replacement asserts an exact match count; a source drift aborts loudly.
# ===========================================================================
import re, sys, os

root = sys.argv[1] if len(sys.argv) > 1 else "."

def load(p):
    with open(p, "r", encoding="utf-8", errors="surrogateescape") as f:
        return f.read()

def save(p, s):
    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)

def sub(text, pattern, repl, expect, tag):
    new, n = re.subn(pattern, repl, text)
    if n != expect:
        sys.stderr.write("[dbg_patch] FATAL: anchor '%s' matched %d times, expected %d\n" % (tag, n, expect))
        sys.exit(2)
    return new

# Non-fatal swallow of a plain `C_PTP (ptp_check_eos_events (params));`.
# Upstream C_PTP does `return translate_ptp_result(rc)` on rc!=PTP_RC_OK;
# here we capture the rc and DISCARD it so init continues (POLARIS_DBG parity).
EOS_SWALLOW = r'{ uint16_t _peos = ptp_check_eos_events (params); (void)_peos; }'
# Non-fatal swallow of `CR (camera_keep_device_on (camera));`.
# Upstream CR does `return rc` on rc<GP_OK; here we discard it.
KEEP_SWALLOW = r'{ int _pkeep = camera_keep_device_on (camera); (void)_pkeep; }'

EOS_ANCHOR  = r'C_PTP \(ptp_check_eos_events \(params\)\);'
KEEP_ANCHOR = r'CR \(camera_keep_device_on \(camera\)\);'

# ---- camlibs/ptp2/config.c : EOS-init drains (13x), all swallowed ----------
cfg = os.path.join(root, "camlibs/ptp2/config.c")
t = load(cfg)
if "_peos" in t:
    print("[dbg_patch] config.c already patched -- skipping")
else:
    t = sub(t, EOS_ANCHOR, EOS_SWALLOW, 13, "config.c check_eos_events non-fatal")
    save(cfg, t)
    print("[dbg_patch] config.c: 13 check_eos_events drains made non-fatal (no trace)")

# ---- camlibs/ptp2/library.c : keep_device_on (3x) + one check_eos_events ----
lib = os.path.join(root, "camlibs/ptp2/library.c")
t = load(lib)
if "_pkeep" in t or "_peos" in t:
    print("[dbg_patch] library.c already patched -- skipping")
else:
    t = sub(t, KEEP_ANCHOR, KEEP_SWALLOW, 3, "library.c keep_device_on non-fatal")
    t = sub(t, EOS_ANCHOR, EOS_SWALLOW, 1, "library.c check_eos_events non-fatal")
    save(lib, t)
    print("[dbg_patch] library.c: 3 keep_device_on + 1 check_eos_events made non-fatal (no trace)")

print("[dbg_patch] DONE -- POLARIS_DBG EOS-init non-fatal (production, no tracing) applied")
