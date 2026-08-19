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


# ---- camlibs/ptp2/config.c : EOS CaptureDestination visibility + override ----
#
# WHY. Canon's EOS_CaptureDestination (0xD11C) is a BITMASK: 1 = camera card,
# 4 = host. Upstream's camera_canon_eos_update_capture_target() picks the card
# value by scanning the enum for the FIRST entry that is not 4:
#
#     if (SupportedValue[i].u32 != PTP_CANON_EOS_CAPTUREDEST_HD) { cardval = ...; break; }
#
# On the R5 Mark II that lands on 0x1 -- card ONLY. With the host absent from
# the destination mask the camera writes its own card and never emits
# ObjectAdded, so libgphoto2 has nothing to announce and Benro's lapse task
# times out at delayMax (~7.03 s) and reports state:-1. Hardware trace
# 2026-08-18 confirmed: shutter fires, image lands on the CF Express card,
# CAPTURE_COMPLETE arrives at t+0.57s, ObjectAdded never does.
#
# The body advertised FIVE supported destinations ("prop d11c options changed,
# type 3, count 5"), but upstream logs none of them and type-3 descriptors are
# not dumped, so we cannot see which combined value exists. This patch:
#   1. logs every supported value (so we learn the real enum on hardware), and
#   2. honours POLARIS_EOS_CAPTUREDEST to force a destination (e.g. 0x5 =
#      card|host), so a value can be tried WITHOUT another cross-build.
#
# Purely additive: with the env unset, behaviour is byte-identical to upstream
# apart from the extra GP_LOG_D lines.
DEST_ANCHOR = r'\tif \(value == 1\)\n\t\tvalue = cardval;'
DEST_PATCH = (
    '\t{\t/* POLARIS: capture-destination visibility + override */\n'
    '\t\tconst char *_pdst = getenv("POLARIS_EOS_CAPTUREDEST");\n'
    '\t\tif (dpd.FormFlag == PTP_DPFF_Enumeration) {\n'
    '\t\t\tunsigned int _pi;\n'
    '\t\t\tfor (_pi=0;_pi<dpd.FORM.Enum.NumberOfValues;_pi++)\n'
    '\t\t\t\tGP_LOG_D ("POLARIS capturedest supported[%u] = 0x%x", _pi,\n'
    '\t\t\t\t          dpd.FORM.Enum.SupportedValue[_pi].u32);\n'
    '\t\t}\n'
    '\t\tGP_LOG_D ("POLARIS capturedest current = 0x%x, cardval = %d",\n'
    '\t\t          dpd.CurrentValue.u32, cardval);\n'
    '\t\tif (_pdst && *_pdst) {\n'
    '\t\t\tcardval = (int)strtol(_pdst, NULL, 0);\n'
    '\t\t\tGP_LOG_D ("POLARIS capturedest OVERRIDE -> 0x%x", cardval);\n'
    '\t\t}\n'
    '\t}\n'
    '\tif (value == 1)\n'
    '\t\tvalue = cardval;'
)

t = load(cfg)
if "POLARIS capturedest" in t:
    print("[dbg_patch] config.c capturedest already patched -- skipping")
else:
    t = sub(t, DEST_ANCHOR, DEST_PATCH.replace('\\', '\\\\'), 1, "config.c capturedest override")
    save(cfg, t)
    print("[dbg_patch] config.c: capturedest enum logging + POLARIS_EOS_CAPTUREDEST override")


# ---- camlibs/ptp2/config.c : declare host capacity for COMBINED destinations --
#
# WHY. EOS_CaptureDestination is a bitmask (1=CFexpress, 2=SD, 4=host), so 0x5
# means "card AND host" -- the mode that writes the camera's own card and still
# hands the image to us. Upstream only ever declares host storage capacity when
# the destination is EXACTLY host:
#
#     if (ct_val.u32 == PTP_CANON_EOS_CAPTUREDEST_HD) { ptp_canon_eos_pchddcapacity(...) }
#
# With 0x5 that test is false, so the camera has the host in its destination
# mask but was never told the host has room -- and refuses the shutter with
# 0x2019 PTP Device Busy (surfaced as -110 'I/O in progress'). Hardware trace
# 2026-08-18: "Canon EOS Full-Press failed (0x2019: PTP Device Busy)".
#
# Worse, the capacity call also sits inside `if (ct_val != CurrentValue)`, so
# once the property is already 0x5 from a previous run the declaration is
# skipped a second way -- the camera can never recover on its own.
#
# So: declare capacity whenever the host BIT is set and the destination is not
# the plain-host case upstream already handles (avoids doing it twice for 0x4).
#
# The upstream wait for AvailableShots>0 is `while (1)` with no bound. We use a
# bounded loop instead: polestar_app watchdogs pgphoto at ~5s, and an unbounded
# spin here would crash-loop the daemon rather than fail one frame.
CAP_ANCHOR = r'\t/\* otherwise we get DeviceBusy for some reason \*/\n\tif \(ct_val\.u32 != dpd\.CurrentValue\.u32\) \{'
CAP_PATCH = (
    '\t/* POLARIS: host is part of the destination mask -> it must declare capacity,\n'
    '\t * otherwise the body answers Full-Press with 0x2019 PTP Device Busy. */\n'
    '\tif ((ct_val.u32 & PTP_CANON_EOS_CAPTUREDEST_HD) &&\n'
    '\t    (ct_val.u32 != PTP_CANON_EOS_CAPTUREDEST_HD)) {\n'
    '\t\t/* ONCE PER CAMERA SESSION.\n'
    '\t\t *\n'
    '\t\t * Not on every trigger: that drains the EOS event queue live view\n'
    '\t\t * feeds from and regressed live view on hardware.\n'
    '\t\t *\n'
    '\t\t * And NOT gated on AvailableShots==0 either, which was the obvious\n'
    '\t\t * cheap test and is WRONG: AvailableShots reports the CAMERA CARD\'s\n'
    '\t\t * free space, which is essentially always > 0, so the declaration\n'
    '\t\t * never ran in a fresh session and the body answered Full-Press with\n'
    '\t\t * 0x2019 Device Busy (surfaced to the app as state:-110). It only\n'
    '\t\t * appeared to work while a previous session had already declared.\n'
    '\t\t *\n'
    '\t\t * Static lifetime is right: pgphoto is restarted whenever the camera\n'
    '\t\t * session is rebuilt, so this re-arms exactly when it should. */\n'
    '\t\tstatic int _pdeclared = 0;\n'
    '\t\tuint16_t _pret;\n'
    '\t\tif (_pdeclared) {\n'
    '\t\t\tgoto _pcap_done;\n'
    '\t\t}\n'
    '\t\t_pret = ptp_canon_eos_pchddcapacity(params, 0x0fffffff, 0x00001000, 0x00000001);\n'
    '\t\tif (_pret == PTP_RC_DeviceBusy) _pret = PTP_RC_OK;\n'
    '\t\tif (_pret != PTP_RC_OK) {\n'
    '\t\t\tGP_LOG_D ("POLARIS pchddcapacity failed 0x%04x", _pret);\n'
    '\t\t} else {\n'
    '\t\t\tint _ptries = 0;\n'
    '\t\t\tPTPDevicePropDesc _pdpd;\n'
    '\t\t\tmemset (&_pdpd, 0, sizeof(_pdpd));\n'
    '\t\t\twhile (_ptries++ < 40) {\t/* bounded: ~2s, watchdog is ~5s */\n'
    '\t\t\t\tif (PTP_RC_OK != ptp_check_eos_events (params)) break;\n'
    '\t\t\t\tif (PTP_RC_OK != ptp_canon_eos_getdevicepropdesc (params,\n'
    '\t\t\t\t\t\tPTP_DPC_CANON_EOS_AvailableShots, &_pdpd)) break;\n'
    '\t\t\t\tif (_pdpd.CurrentValue.u32 > 0) { ptp_free_devicepropdesc (&_pdpd); break; }\n'
    '\t\t\t\tptp_free_devicepropdesc (&_pdpd);\n'
    '\t\t\t\tusleep (50*1000);\n'
    '\t\t\t}\n'
    '\t\t\t_pdeclared = 1;\n'
    '\t\t\tGP_LOG_D ("POLARIS host capacity declared for dest 0x%x, tries=%d",\n'
    '\t\t\t          ct_val.u32, _ptries);\n'
    '\t\t}\n'
    '\t\t_pcap_done: ;\n'
    '\t}\n'
    '\t/* otherwise we get DeviceBusy for some reason */\n'
    '\tif (ct_val.u32 != dpd.CurrentValue.u32) {'
)

t = load(cfg)
if "POLARIS host capacity" in t:
    print("[dbg_patch] config.c pchddcapacity already patched -- skipping")
else:
    t = sub(t, CAP_ANCHOR, CAP_PATCH.replace('\\', '\\\\'), 1, "config.c combined-dest host capacity")
    save(cfg, t)
    print("[dbg_patch] config.c: host capacity declared for combined capture destinations")

print("[dbg_patch] DONE -- POLARIS_DBG EOS-init non-fatal (production, no tracing) applied")
