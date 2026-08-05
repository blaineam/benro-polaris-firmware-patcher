#!/usr/bin/env python3
# ===========================================================================
#  POLARIS_TRACE diagnostic instrumentation (THROWAWAY — not for shipping).
#
#  Patches the upstream libgphoto2 2.5.34 source, in-tree, to emit unbuffered
#  `fprintf(stderr, "POLARIS_TRACE: ...\n")` at every point on the Canon EOS
#  post-capture event path, so we can see on-device exactly where the
#  GP_EVENT_FILE_ADDED event is (or is not) produced after CAPTURE_COMPLETE.
#
#  Instruments (all lines prefixed "POLARIS_TRACE:"):
#    camlibs/ptp2/library.c  -> camera_wait_for_event (Canon EOS branch)
#    camlibs/ptp2/ptp.c      -> ptp_check_eos_events (EOS event-queue drain)
#    libgphoto2/gphoto2-filesys.c -> gp_filesystem_append (entry/exit)
#
#  Adds NO exported symbols (fprintf/fflush/stderr are libc), so the trampoline
#  boundary set + CAMLIBS loading resolve identically to the normal build.
#
#  Invoked from build_ptp2.sh ONLY when TRACE=1. Idempotent (re-run safe).
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

def sub(text, pattern, builder, expect, tag):
    new, n = re.subn(pattern, builder, text)
    if n != expect:
        sys.stderr.write("[trace_patch] FATAL: anchor '%s' matched %d times, expected %d\n" % (tag, n, expect))
        sys.exit(2)
    return new

def ensure_stdio(text):
    if "<stdio.h>" in text:
        return text
    # inject after the first #include line
    return re.sub(r"(#include[^\n]*\n)", r"\1#include <stdio.h>\n", text, count=1)

# ---------------------------------------------------------------------------
# 1. camlibs/ptp2/library.c  — camera_wait_for_event (Canon EOS branch)
# ---------------------------------------------------------------------------
lib = os.path.join(root, "camlibs/ptp2/library.c")
t = load(lib)
if "POLARIS_TRACE" in t:
    print("[trace_patch] library.c already instrumented -- skipping")
else:
    t = ensure_stdio(t)

    # L1: function ENTRY
    t = sub(t,
        r'(GP_LOG_D \("waiting for events timeout %d ms", timeout\);\n)',
        lambda m: m.group(1) + '\tfprintf(stderr, "POLARIS_TRACE: camera_wait_for_event ENTRY timeout=%d\\n", timeout); fflush(stderr);\n',
        1, "L1 wait_for_event ENTRY")

    # L2: Canon EOS branch entered (just before the drain do-loop)
    t = sub(t,
        r'(\n)([ \t]*)(do \{\n[ \t]*PTPCanonEOSEvent eos_event;\n)',
        lambda m: m.group(1) + m.group(2) + 'fprintf(stderr, "POLARIS_TRACE: wait_for_event CANON_EOS branch entered\\n"); fflush(stderr);\n' + m.group(2) + m.group(3),
        1, "L2 EOS branch entered")

    # L3: after ptp_check_eos_events drain, how many are queued
    t = sub(t,
        r'(if \(params->eos_events\.len == 0\)\n[ \t]*C_PTP_REP_MSG \(ptp_check_eos_events \(params\), _\("Canon EOS Get Changes failed"\)\);\n)',
        lambda m: m.group(1) + '\t\t\tfprintf(stderr, "POLARIS_TRACE: wait_for_event queued=%u after check_eos_events\\n", (unsigned)params->eos_events.len); fflush(stderr);\n',
        1, "L3 post check_eos_events")

    # L4: each event dequeued
    t = sub(t,
        r'(while \(ptp_get_one_eos_event \(params, &eos_event\)\) \{\n[ \t]*GP_LOG_D \("processing event \'%s\'", ptp_get_eos_event_name\(params, eos_event\.type\)\);\n)',
        lambda m: m.group(1) + '\t\t\t\tfprintf(stderr, "POLARIS_TRACE: dequeued EOS event type=%d name=%s\\n", (int)eos_event.type, ptp_get_eos_event_name(params, eos_event.type)); fflush(stderr);\n',
        1, "L4 dequeue")

    # L5: ObjectTransfer -> gp_filesystem_append (download path) call + ret.
    #     Anchored on the unique preceding ptp_canon_eos_transfercomplete call so
    #     we hit ONLY the ObjectTransfer-case append (7 identical append lines exist).
    t = sub(t,
        r'(ptp_canon_eos_transfercomplete \(params, eos_event\.u\.object\.Handle\)\);\n)([\s\S]*?)(ret = gp_filesystem_append\(camera->fs, path->folder, path->name, context\);\n)',
        lambda m: m.group(1) + m.group(2)
                  + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: ObjectTransfer -> gp_filesystem_append folder=%s name=%s\\n", path->folder, path->name); fflush(stderr);\n'
                  + m.group(3)
                  + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: ObjectTransfer gp_filesystem_append ret=%d\\n", ret); fflush(stderr);\n',
        1, "L5 ObjectTransfer append")

    # L6: ObjectTransfer sets FILE_ADDED (return GP_OK)
    t = sub(t,
        r'(gp_filesystem_set_info_noop\(camera->fs, path->folder, path->name, info, context\);\n[ \t]*\*eventtype = GP_EVENT_FILE_ADDED;\n[ \t]*\*eventdata = path;\n)',
        lambda m: m.group(1) + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: ObjectTransfer -> *eventtype=GP_EVENT_FILE_ADDED path=%s/%s (return GP_OK)\\n", path->folder, path->name); fflush(stderr);\n',
        1, "L6 ObjectTransfer FILE_ADDED")

    # L7: ObjectInfo/ContentChanged -> FILE_CHANGED
    t = sub(t,
        r'(GP_LOG_D \("object content changed: handle 0x%x", eos_event\.u\.object\.Handle\);\n)',
        lambda m: m.group(1) + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: EOS ObjectInfo/ContentChanged handle=0x%x -> GP_EVENT_FILE_CHANGED\\n", eos_event.u.object.Handle); fflush(stderr);\n',
        1, "L7 ObjectInfoChanged")

    # L8: ObjectAdded event log
    t = sub(t,
        r'(case PTP_EOSEvent_ObjectAdded: \{\n[ \t]*GP_LOG_D \("object added: handle 0x%x, name %s", eos_event\.u\.object\.Handle, eos_event\.u\.object\.Filename\);\n)',
        lambda m: m.group(1) + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: EOS ObjectAdded handle=0x%x name=%s\\n", eos_event.u.object.Handle, eos_event.u.object.Filename); fflush(stderr);\n',
        1, "L8 ObjectAdded log")

    # L9: ObjectAdded -> add_object_to_fs_and_path ret
    t = sub(t,
        r'(ret = add_object_to_fs_and_path \(camera, eos_event\.u\.object\.Handle, path, context\);\n)',
        lambda m: m.group(1) + '\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: ObjectAdded add_object_to_fs_and_path ret=%d folder=%s name=%s\\n", ret, path->folder, path->name); fflush(stderr);\n',
        1, "L9 ObjectAdded add_object")

    # L10: ObjectAdded final *eventtype + return GP_OK
    t = sub(t,
        r'(\} else \{\n[ \t]*\*eventtype = GP_EVENT_FILE_ADDED;\n[ \t]*\}\n)([ \t]*)(\*eventdata = path;\n[ \t]*return GP_OK;\n[ \t]*\}\n[ \t]*case PTP_EOSEvent_PropertyChanged:)',
        lambda m: m.group(1) + m.group(2) + 'fprintf(stderr, "POLARIS_TRACE: ObjectAdded -> *eventtype=%s path=%s/%s (return GP_OK)\\n", (*eventtype==GP_EVENT_FOLDER_ADDED)?"GP_EVENT_FOLDER_ADDED":"GP_EVENT_FILE_ADDED", path->folder, path->name); fflush(stderr);\n' + m.group(2) + m.group(3),
        1, "L10 ObjectAdded FILE_ADDED return")

    # L11: CameraStatus==0 -> CAPTURE_COMPLETE
    t = sub(t,
        r'(if \(eos_event\.u\.status == 0\) \{\n[ \t]*\*eventtype = GP_EVENT_CAPTURE_COMPLETE;\n)',
        lambda m: m.group(1) + '\t\t\t\t\t\tfprintf(stderr, "POLARIS_TRACE: EOS CameraStatus==0 -> GP_EVENT_CAPTURE_COMPLETE\\n"); fflush(stderr);\n',
        1, "L11 CAPTURE_COMPLETE")

    # L12: EOS drain loop timed out -> TIMEOUT. Anchored from the unique
    #      "Unhandled EOS event" default-case log down to this branch's timeout
    #      return (6 vendor branches share the bare timeout tail; this is the EOS one).
    t = sub(t,
        r'(GP_LOG_D \("Unhandled EOS event 0x%04x", eos_event\.type\);\n)'
        r'([\s\S]*?\} while \(waiting_for_timeout \(&back_off_wait, event_start, timeout\)\);\n\n)'
        r'([ \t]*)(\*eventtype = GP_EVENT_TIMEOUT;\n[ \t]*return GP_OK;)',
        lambda m: m.group(1) + m.group(2) + m.group(3) + 'fprintf(stderr, "POLARIS_TRACE: CANON_EOS drain loop TIMEOUT -> GP_EVENT_TIMEOUT (no file this call)\\n"); fflush(stderr);\n' + m.group(3) + m.group(4),
        1, "L12 EOS TIMEOUT")

    # -----------------------------------------------------------------------
    #  POLARIS_DBG parity (EOS-init non-fatal) + EOS-init tracing in library.c.
    #  Recreates Benro's POLARIS_DBG behavior that the WORKING debug ptp2
    #  (md5 e7f60908, strings "POLARIS_DBG check_eos_events non-fatal=0x%x" and
    #  "POLARIS_DBG keep_device_on non-fatal=%d") had and the clean TRACED build
    #  lacked: the failing Canon EOS-init commands must NOT abort camera_init,
    #  or gphoto discards the real Canon EOS driver and falls back to the generic
    #  "USB PTP Class Camera" (error 0x2002 = PTP_RC_GeneralError seen x3).
    #
    #  The two POLARIS_DBG format specifiers pin the exact call each wraps:
    #    * check_eos_events -> "0x%x" == the uint16 PTP RC returned by
    #      ptp_check_eos_events; i.e. the plain `C_PTP (ptp_check_eos_events(params));`
    #      (C_PTP does `return translate_ptp_result(rc)` on !=PTP_RC_OK).
    #    * keep_device_on   -> "%d"  == the signed gphoto int returned by
    #      camera_keep_device_on; i.e. the `CR (camera_keep_device_on(camera));`
    #      call site (CR does `return rc` on <0).
    #  NOTE the capture/event path uses `C_PTP_REP_MSG (ptp_check_eos_events(...),
    #  _("Canon EOS Get Changes failed"))` (a DIFFERENT, message-carrying form that
    #  the single POLARIS_DBG string does not cover) -- so the transform below is
    #  scoped to the plain `C_PTP (...)` form ONLY and leaves the capture-path
    #  drains (and their existing L1-L12 / P1 traces) fatal and untouched.

    # L13: `CR (camera_keep_device_on (camera));` -> log-and-continue (non-fatal),
    #      matching POLARIS_DBG "keep_device_on non-fatal=%d".
    t = sub(t,
        r'CR \(camera_keep_device_on \(camera\)\);',
        lambda m: ('{ int _pkeep = camera_keep_device_on (camera); if (_pkeep < GP_OK) { '
                   'fprintf(stderr, "POLARIS_TRACE: eos_init keep_device_on ret=%d (non-fatal continue)\\n", _pkeep); fflush(stderr); } }'),
        3, "L13 keep_device_on non-fatal")

    # L14: plain `C_PTP (ptp_check_eos_events (params));` -> log-and-continue,
    #      matching POLARIS_DBG "check_eos_events non-fatal=0x%x".
    t = sub(t,
        r'C_PTP \(ptp_check_eos_events \(params\)\);',
        lambda m: ('{ uint16_t _peos = ptp_check_eos_events (params); '
                   'fprintf(stderr, "POLARIS_TRACE: eos_init check_eos_events ret=0x%x%s\\n", _peos, _peos!=PTP_RC_OK?" (non-fatal continue)":""); fflush(stderr); }'),
        1, "L14 check_eos_events non-fatal (library.c)")

    save(lib, t)
    print("[trace_patch] library.c instrumented (12 anchors + L13 keep_device_on + L14 check_eos_events non-fatal)")

# ---------------------------------------------------------------------------
# 2. camlibs/ptp2/ptp.c — ptp_check_eos_events (EOS event-queue drain)
# ---------------------------------------------------------------------------
p = os.path.join(root, "camlibs/ptp2/ptp.c")
t = load(p)
if "POLARIS_TRACE" in t:
    print("[trace_patch] ptp.c already instrumented -- skipping")
else:
    t = ensure_stdio(t)
    t = sub(t,
        r'(while \(1\) \{ /\* call it repeatedly until the camera does not report any \*/\n)'
        r'([ \t]*CHECK_PTP_RC\(ptp_canon_eos_getevent \(params, &events\)\);\n)'
        r'([ \t]*)if \(!events\.len\)\n'
        r'[ \t]*return PTP_RC_OK;\n'
        r'\n'
        r'([ \t]*)(array_append\(&params->eos_events, &events\);\n)',
        lambda m: (m.group(1) + m.group(2)
            + m.group(3) + 'if (!events.len) {\n'
            + m.group(3) + '\tfprintf(stderr, "POLARIS_TRACE: ptp_check_eos_events: no more events, total queued=%u\\n", (unsigned)params->eos_events.len); fflush(stderr);\n'
            + m.group(3) + '\treturn PTP_RC_OK;\n'
            + m.group(3) + '}\n'
            + m.group(4) + 'fprintf(stderr, "POLARIS_TRACE: ptp_check_eos_events: batch len=%u\\n", (unsigned)events.len); fflush(stderr);\n'
            + m.group(4) + 'for (uint32_t _pi = 0; _pi < events.len; _pi++)\n'
            + m.group(4) + '\tfprintf(stderr, "POLARIS_TRACE:   eos batch[%u] type=%d name=%s\\n", _pi, (int)events.val[_pi].type, ptp_get_eos_event_name(params, events.val[_pi].type));\n'
            + m.group(4) + 'fflush(stderr);\n'
            + m.group(4) + m.group(5)),
        1, "P1 ptp_check_eos_events")
    save(p, t)
    print("[trace_patch] ptp.c instrumented (1 anchor)")

# ---------------------------------------------------------------------------
# 3. libgphoto2/gphoto2-filesys.c — gp_filesystem_append (entry/exit)
# ---------------------------------------------------------------------------
fsy = os.path.join(root, "libgphoto2/gphoto2-filesys.c")
t = load(fsy)
if "POLARIS_TRACE" in t:
    print("[trace_patch] gphoto2-filesys.c already instrumented -- skipping")
else:
    t = ensure_stdio(t)

    # F1: ENTRY
    t = sub(t,
        r'(GP_LOG_D \("Append %s/%s to filesystem", folder, filename\);\n)',
        lambda m: m.group(1) + '\tfprintf(stderr, "POLARIS_TRACE: gp_filesystem_append ENTRY folder=%s filename=%s\\n", folder, filename?filename:"(null)"); fflush(stderr);\n',
        1, "F1 append ENTRY")

    # F2: files_dirty reload (capture case) — reload + its ret
    t = sub(t,
        r'(if \(f->files_dirty\) \{ /\* Need to load folder from driver first \.\.\. capture case \*/\n)'
        r'([ \t]*CameraList[ \t]+\*xlist;\n[ \t]*int ret;\n\n)'
        r'([ \t]*CR \(gp_list_new \(&xlist\)\);\n)'
        r'([ \t]*ret = gp_filesystem_list_files \(fs, folder, xlist, context\);\n)'
        r'([ \t]*gp_list_free \(xlist\);\n)',
        lambda m: (m.group(1) + m.group(2)
            + '\t\tfprintf(stderr, "POLARIS_TRACE: gp_filesystem_append folder=%s files_dirty -> reload (capture case)\\n", folder); fflush(stderr);\n'
            + m.group(3) + m.group(4) + m.group(5)
            + '\t\tfprintf(stderr, "POLARIS_TRACE: gp_filesystem_append folder=%s reload ret=%d\\n", folder, ret); fflush(stderr);\n'),
        1, "F2 files_dirty reload")

    # F3: EXIT (final return)
    t = sub(t,
        r'(ret = internal_append \(fs, f, filename, context\);\n[ \t]*if \(ret == GP_ERROR_FILE_EXISTS\)[^\n]*\n[ \t]*ret = GP_OK;\n)([ \t]*)(return ret;)',
        lambda m: m.group(1) + m.group(2) + 'fprintf(stderr, "POLARIS_TRACE: gp_filesystem_append EXIT folder=%s filename=%s ret=%d\\n", folder, filename?filename:"(null)", ret); fflush(stderr);\n' + m.group(2) + m.group(3),
        1, "F3 append EXIT")

    save(fsy, t)
    print("[trace_patch] gphoto2-filesys.c instrumented (3 anchors)")

# ---------------------------------------------------------------------------
# 4. camlibs/ptp2/config.c — Canon EOS INIT path.
#
#  camera_prepare_canon_eos_capture() (config.c) IS the Canon EOS bring-up the
#  device runs during gp_camera_init: setremotemode -> seteventmode -> the
#  "initial bulk" ptp_check_eos_events drain -> getdeviceinfo -> more drains.
#  On the R5 Mark II the check_eos_events drains return 0x2002 (GeneralError);
#  upstream's `C_PTP (...)` makes that FATAL, camera_init returns an error, and
#  gphoto falls back to the generic "USB PTP Class Camera" driver (the current
#  clean TRACED build's symptom). Benro's POLARIS_DBG swallowed exactly the
#  check_eos_events (and keep_device_on) failures so init completes and the real
#  'Canon EOS R5m2' abilities stick. We reproduce that here AND trace every
#  EOS-init command's return code so the device log shows which one returns
#  0x2002 and confirms it is now swallowed.
#
#  Non-fatal (matches the two POLARIS_DBG strings): ptp_check_eos_events.
#  Fatal-but-traced (NOT swallowed by POLARIS_DBG -- no string for them; they
#  must be succeeding on the R5m2, and if the trace ever shows one returning
#  0x2002 we'd know to swallow it too): setremotemode, seteventmode.
# ---------------------------------------------------------------------------
cfg = os.path.join(root, "camlibs/ptp2/config.c")
t = load(cfg)
if "POLARIS_TRACE" in t:
    print("[trace_patch] config.c already instrumented -- skipping")
else:
    t = ensure_stdio(t)

    # C1: every plain `C_PTP (ptp_check_eos_events (params));` -> log-and-continue
    #     (non-fatal), matching POLARIS_DBG "check_eos_events non-fatal=0x%x".
    #     Covers the EOS-init drains (399/429/434/457/489) plus the other plain
    #     drains in config.c. The message-carrying C_PTP_REP_MSG capture-path
    #     drains do NOT match this anchor and stay fatal + traced by ptp.c/P1.
    t = sub(t,
        r'C_PTP \(ptp_check_eos_events \(params\)\);',
        lambda m: ('{ uint16_t _peos = ptp_check_eos_events (params); '
                   'fprintf(stderr, "POLARIS_TRACE: eos_init check_eos_events ret=0x%x%s\\n", _peos, _peos!=PTP_RC_OK?" (non-fatal continue)":""); fflush(stderr); }'),
        13, "C1 config.c check_eos_events non-fatal")

    # C2: trace ptp_canon_eos_setremotemode return, KEEP FATAL (upstream C_PTP
    #     semantics). Shows if setremotemode is the 0x2002 source.
    t = sub(t,
        r'C_PTP \(ptp_canon_eos_setremotemode\(params, ([^)]*)\)\);',
        lambda m: ('{ uint16_t _prm = ptp_canon_eos_setremotemode(params, ' + m.group(1) + '); '
                   'fprintf(stderr, "POLARIS_TRACE: eos_init setremotemode(arg=%d) ret=0x%x\\n", (int)(' + m.group(1) + '), _prm); fflush(stderr); '
                   'if (_prm != PTP_RC_OK) return translate_ptp_result(_prm); }'),
        4, "C2 config.c setremotemode trace")

    # C3: trace ptp_canon_eos_seteventmode return, KEEP FATAL. Shows if
    #     seteventmode is the 0x2002 source. (The space-variant `seteventmode
    #     (params, mode)` at ~8697 intentionally does NOT match -> stays as-is.)
    t = sub(t,
        r'C_PTP \(ptp_canon_eos_seteventmode\(params, ([^)]*)\)\);',
        lambda m: ('{ uint16_t _pev = ptp_canon_eos_seteventmode(params, ' + m.group(1) + '); '
                   'fprintf(stderr, "POLARIS_TRACE: eos_init seteventmode(arg=%d) ret=0x%x\\n", (int)(' + m.group(1) + '), _pev); fflush(stderr); '
                   'if (_pev != PTP_RC_OK) return translate_ptp_result(_pev); }'),
        3, "C3 config.c seteventmode trace")

    save(cfg, t)
    print("[trace_patch] config.c instrumented (C1 check_eos_events non-fatal x13, C2 setremotemode x4, C3 seteventmode x3)")

print("[trace_patch] DONE -- POLARIS_TRACE instrumentation applied")
