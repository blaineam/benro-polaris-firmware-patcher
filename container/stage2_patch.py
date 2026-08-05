#!/usr/bin/env python3
"""
stage2_patch.py -- Full-libgphoto2 (Stage 2) ON-DISK trampoline patcher.

This is the DEFAULT-mode patcher.  It takes the RELIABILITY-PATCHED base pgphoto
(stock + the 14-byte reliability patch produced by analyze_pgphoto.py --apply) and
redirects the 64 internal libgphoto2 *boundary* entry points (`gp_*` / `gp_port_*`
the Benro binary calls) into a freshly-built 2.5.34 core/port, WITHOUT touching
`.text` at runtime.

Why on-disk (not a runtime .text loader): three different RUNTIME `.text`
self-modifying loaders (span RWX, per-page RWX, W^X two-step) each crashed or were
refused by the device's Hi3559V200 kernel (mprotect ENOMEM / /proc/self/mem
segfault), and NONE of those failures reproduce under qemu-user.  This patcher
removes runtime `.text` mprotect ENTIRELY: it edits the pgphoto FILE so each
boundary entry is an absolute INDIRECT jump through a per-entry pointer slot that
lives in a fresh anonymous page the loader mmaps MAP_FIXED at SLOT_BASE.  The
loader then only writes the slots (plain stores to writable data -- no mprotect,
no `.text` write, no /proc/self/mem).

Mechanism (all constant / known at patch time; nothing written to `.text` at
runtime):

  1. The 64 pointer slots live at SLOT_BASE + i*4 (SLOT_BASE fixed, below).
     pgphoto is a non-PIE EXEC, so the entry vaddrs (and hence the `.word slot`
     constants) are fixed.  The patcher statically refuses if the slot region
     overlaps any of pgphoto's own PT_LOADs; the loader independently confirms the
     region is free (via /proc/self/maps) before MAP_FIXED-ing it.

  2. Overwrite the first 12 bytes of each boundary entry (>=56 bytes each, so 12
     fits with huge margin) with an absolute indirect jump through its slot:

         entry+0:  E59FC000   ldr r12, [pc, #0]   ; r12 = *(entry+8) = &slot_i
         entry+4:  E59CF000   ldr pc,  [r12]       ; pc = *slot_i (target)
         entry+8:  <slot_i vaddr>                  ; CONSTANT, known now

     (ARM pc reads as insn_addr+8, so the `ldr r12,[pc,#0]` at entry+0 loads the
     word at entry+8.)  This edit is in the FILE only; `.text` is never mprotected
     at runtime.

  3. Emit stage2_ondisk_table.h ({name, slot_vaddr}) for the loader, plus an
     auditable manifest.

Fail-closed: any entry <12 bytes, any address not inside an executable LOAD, any
boundary symbol not exported by the new core/port lib, a reliability-patch byte
that would be clobbered by a trampoline (see --reliability-base), or (with
--expect-md5) an unexpected input md5 -> nonzero exit, no output written.

Ships NO Benro/reconstructed source: the boundary is the list of public LGPL
libgphoto2 API names the binary calls; the loader/patcher are generic MIT.  The
tool patches the USER's OWN extracted pgphoto -- no proprietary/firmware bytes
are shipped.
"""
import argparse, hashlib, json, os, struct, subprocess, sys

# --- Stage 2 core boundary.  gp_params_* are EXCLUDED (gphoto2-CLI code compiled
#     into pgphoto and Benro-extended; they stay stock, never trampolined). ------
CORE_BOUNDARY = [
    "gp_abilities_list_detect","gp_abilities_list_get_abilities",
    "gp_abilities_list_lookup_model","gp_camera_capture",
    "gp_camera_capture_preview","gp_camera_exit","gp_camera_file_delete",
    "gp_camera_file_get","gp_camera_file_get_info","gp_camera_folder_list_files",
    "gp_camera_folder_make_dir","gp_camera_folder_remove_dir",
    "gp_camera_get_abilities","gp_camera_get_config","gp_camera_get_port_info",
    "gp_camera_get_summary","gp_camera_init","gp_camera_set_config",
    "gp_camera_set_single_config","gp_camera_set_timeout_funcs",
    "gp_camera_trigger_capture","gp_camera_unref","gp_camera_wait_for_event",
    "gp_context_error","gp_context_unref","gp_file_get_data_and_size",
    "gp_file_get_mtime","gp_file_get_name_by_type","gp_file_new",
    "gp_file_new_from_fd","gp_file_new_from_handler","gp_file_unref",
    "gp_filesystem_get_file","gp_list_count","gp_list_free","gp_list_get_name",
    "gp_list_get_value","gp_list_new","gp_log","gp_result_as_string",
    "gp_setting_get","gp_system_is_dir","gp_system_is_file","gp_system_mkdir",
    "gp_widget_count_choices","gp_widget_free","gp_widget_get_child_by_name",
    "gp_widget_get_choice","gp_widget_get_name","gp_widget_get_range",
    "gp_widget_get_readonly","gp_widget_get_type","gp_widget_get_value",
    "gp_widget_set_value","gp_widget_unref",
]
PORT_BOUNDARY = [
    "gp_port_close","gp_port_free","gp_port_info_get_name","gp_port_info_get_path",
    "gp_port_info_get_type","gp_port_new","gp_port_open","gp_port_reset",
    "gp_port_set_info",
]
BOUNDARY = CORE_BOUNDARY + PORT_BOUNDARY

# Reference (FwVer 4.0.0.32) md5s, for informational logging only.  The patcher is
# generic -- it fails closed on the STRUCTURAL guards below, not on a hard-coded
# md5 -- so it works on any structurally-identical firmware.  Pass --expect-md5 to
# enforce a specific input.
KNOWN_STOCK_MD5        = "a766aaf9320978e271d1d8cf0f1677a5"
KNOWN_BASE_MD5         = "80678d87993ca042097986c761f90ca2"   # reliability-patched
KNOWN_STAGE2_OUT_MD5   = "a83ac7bbee13078ca53807a452961285"   # hardware-validated

TRAMP_BYTES = 12
ARM_LDR_R12_PC0 = 0xE59FC000   # ldr r12, [pc, #0]
ARM_LDR_PC_R12  = 0xE59CF000   # ldr pc,  [r12]
SLOT_SZ     = 4

# Fixed slot-region base vaddr.  The 64 slots do NOT live in pgphoto's memory image
# (an extended-.bss slot region was NOT reliably mapped writable on the Hi3559V200
# kernel -- the loader's first slot write nondeterministically faulted).  Instead
# they live in a FRESH anonymous page the loader mmaps MAP_FIXED at SLOT_BASE as its
# first action.  SLOT_BASE sits in the wide free gap between pgphoto's
# text+data+bss+heap (~0x10000..~0x37d000+heap) and the shared libraries (~0xb6xx).
# The patcher statically refuses if SLOT_BASE overlaps any pgphoto PT_LOAD; the
# loader additionally reads /proc/self/maps and refuses (rather than clobbering)
# if anything already occupies it.
SLOT_BASE   = 0x30000000


def die(msg):
    sys.stderr.write("[stage2] FATAL: %s\n" % msg)
    sys.exit(2)


def elf_functable(nm, path):
    out = subprocess.check_output([nm, "-S", "--defined-only", path],
                                  universal_newlines=True)
    t = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[2] in ("T", "t"):        # value size type name
            t[p[3]] = (int(p[0], 16), int(p[1], 16))
    return t


def dynsyms(readelf, path):
    out = subprocess.check_output([readelf, "--dyn-syms", "-W", path],
                                  universal_newlines=True)
    s = set()
    for ln in out.splitlines():
        p = ln.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
            s.add(p[7].split("@")[0])
    return s


class Elf32:
    """Minimal 32-bit LE ELF program/section-header reader."""
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            die("not a 32-bit LE ELF")
        self.e_type,      = struct.unpack_from("<H", d, 0x10)
        self.e_machine,   = struct.unpack_from("<H", d, 0x12)
        self.e_entry,     = struct.unpack_from("<I", d, 0x18)
        self.e_phoff,     = struct.unpack_from("<I", d, 0x1c)
        self.e_shoff,     = struct.unpack_from("<I", d, 0x20)
        self.e_phentsize, self.e_phnum = struct.unpack_from("<HH", d, 0x2a)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from("<HHH", d, 0x2e)
        if self.e_machine != 40:
            die("not ARM (EM_ARM)")

    def phdrs(self):
        for i in range(self.e_phnum):
            o = self.e_phoff + i * self.e_phentsize
            (p_type, p_off, p_va, p_pa, p_fsz, p_msz,
             p_flags, p_align) = struct.unpack_from("<IIIIIIII", self.d, o)
            yield i, o, dict(type=p_type, off=p_off, va=p_va, fsz=p_fsz,
                             msz=p_msz, flags=p_flags, align=p_align)

    def va_to_off(self, va):
        """Map a vaddr into a file offset via the LOAD segment containing it."""
        for _, _, p in self.phdrs():
            if p["type"] == 1 and p["va"] <= va < p["va"] + p["fsz"]:
                return p["off"] + (va - p["va"]), p
        return None, None


def reliability_ranges(base_bytes, stock_path):
    """Generic reliability-site guard: the bytes that differ between the stock
    pgphoto and the reliability-patched base ARE the reliability-patch sites
    (resetUsb / list-files / gates).  Return a list of (file_off, length) runs of
    contiguous differing bytes.  No device-specific addresses are hard-coded -- the
    sites are DERIVED from the user's own two binaries."""
    stock = open(stock_path, "rb").read()
    if len(stock) != len(base_bytes):
        die("reliability-base %s and base differ in size (%d vs %d) -- not the "
            "same pgphoto build" % (stock_path, len(stock), len(base_bytes)))
    runs, i, n = [], 0, len(stock)
    while i < n:
        if stock[i] != base_bytes[i]:
            j = i
            while j < n and stock[j] != base_bytes[j]:
                j += 1
            runs.append((i, j - i))
            i = j
        else:
            i += 1
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pgphoto", required=True,
                    help="the RELIABILITY-PATCHED base pgphoto "
                         "(analyze_pgphoto.py --apply output)")
    ap.add_argument("--core", required=True, help="new libgphoto2.so.6")
    ap.add_argument("--port", required=True, help="new libgphoto2_port.so.12")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--readelf", default="readelf")
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--out", default=None, help="patched binary path "
                    "(default OUTDIR/pgphoto.stage2ondisk)")
    ap.add_argument("--reliability-base", default=None,
                    help="the ORIGINAL stock pgphoto; if given, the bytes that "
                         "differ between it and --pgphoto are treated as the "
                         "reliability-patch sites and the patcher fails closed if "
                         "any trampoline would clobber one (fail-closed guard)")
    ap.add_argument("--expect-md5", default=None,
                    help="require this exact input md5 (optional; the structural "
                         "guards below are the real safety net)")
    ap.add_argument("--allow-md5", action="store_true",
                    help="do not fail on --expect-md5 mismatch")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    out_bin = a.out or os.path.join(a.outdir, "pgphoto.stage2ondisk")

    raw = open(a.pgphoto, "rb").read()
    in_md5 = hashlib.md5(raw).hexdigest()
    if a.expect_md5 and in_md5 != a.expect_md5 and not a.allow_md5:
        die("input md5 %s != expected %s (use --allow-md5 to override)"
            % (in_md5, a.expect_md5))

    e = Elf32(raw)
    if e.e_type != 2:
        die("pgphoto must be ET_EXEC (non-PIE) for fixed slot/entry vaddrs; "
            "got e_type=%d" % e.e_type)

    # --- fixed slot region at SLOT_BASE (NO .bss/segment size changes). ---------
    slot_base = SLOT_BASE
    n = len(BOUNDARY)
    slot_region = n * SLOT_SZ
    slot_end = slot_base + slot_region
    for i, _, p in e.phdrs():
        if p["type"] != 1:                       # PT_LOAD only
            continue
        seg_lo, seg_hi = p["va"], p["va"] + p["msz"]
        if slot_base < seg_hi and slot_end > seg_lo:
            die("slot region [%#x,%#x) overlaps pgphoto PT_LOAD #%d [%#x,%#x); "
                "pick a different SLOT_BASE" % (slot_base, slot_end, i, seg_lo, seg_hi))

    # --- reliability sites (derived; fail-closed collision guard). --------------
    rel_report = {"checked": False, "sites": []}
    rel_ranges = []
    if a.reliability_base:
        rel_ranges = reliability_ranges(e.d, a.reliability_base)
        rel_report["checked"] = True
        for (ro, rl) in rel_ranges:
            rel_report["sites"].append({"file_off": "0x%x" % ro, "len": rl})
        total = sum(rl for _, rl in rel_ranges)
        sys.stderr.write("[stage2] reliability sites derived from stock<->base "
                         "diff: %d run(s), %d byte(s)\n" % (len(rel_ranges), total))

    # --- resolve every boundary entry against the base symtab + new libs. -------
    ft = elf_functable(a.nm, a.pgphoto)
    core_exp = dynsyms(a.readelf, a.core)
    port_exp = dynsyms(a.readelf, a.port)

    entries, blockers = [], []
    for idx, name in enumerate(BOUNDARY):
        rec = {"symbol": name, "slot_index": idx,
               "slot_vaddr": "0x%08x" % (slot_base + idx * SLOT_SZ)}
        if name not in ft:
            rec["status"] = "MISSING_IN_PGPHOTO"; blockers.append(rec)
            entries.append(rec); continue
        addr, size = ft[name]
        rec["stock_addr"] = "0x%08x" % addr
        rec["stock_size"] = size
        foff, seg = e.va_to_off(addr)
        if foff is None:
            rec["status"] = "ADDR_NOT_IN_FILE"; blockers.append(rec)
            entries.append(rec); continue
        if not (seg["flags"] & 0x1):                 # PF_X
            rec["status"] = "ADDR_NOT_EXECUTABLE"; blockers.append(rec)
            entries.append(rec); continue
        rec["file_off"] = "0x%08x" % foff
        rec["orig12"] = e.d[foff:foff + TRAMP_BYTES].hex()
        exp = name in core_exp
        soname = os.path.basename(a.core)
        if not exp and name in port_exp:
            exp = True; soname = os.path.basename(a.port)
        rec["target_lib"] = soname if exp else None
        if size < TRAMP_BYTES:
            rec["status"] = "ENTRY_TOO_SMALL"; blockers.append(rec)
        elif not exp:
            rec["status"] = "UNRESOLVED_IN_NEWLIB"; blockers.append(rec)
        else:
            rec["status"] = "OK"
        entries.append(rec)

    if blockers:
        sys.stderr.write("[stage2] %d blocker(s); NOT patching:\n" % len(blockers))
        for b in blockers:
            sys.stderr.write("  BLOCKER %-32s %s\n" % (b["symbol"], b["status"]))
        json.dump({"input": a.pgphoto, "in_md5": in_md5, "ok": False,
                   "blockers": len(blockers), "entries": entries},
                  open(os.path.join(a.outdir, "stage2_ondisk_manifest.json"), "w"),
                  indent=2)
        sys.exit(1)

    # --- reliability collision guard (fail-closed) BEFORE any write. ------------
    if rel_ranges:
        for rec in entries:
            foff = int(rec["file_off"], 16)
            for (ro, rl) in rel_ranges:
                if foff < ro + rl and foff + TRAMP_BYTES > ro:
                    die("COLLISION: trampoline for %s [%#x,%#x) would overwrite a "
                        "reliability-patch site [%#x,%#x) -- refusing to patch"
                        % (rec["symbol"], foff, foff + TRAMP_BYTES, ro, ro + rl))
        sys.stderr.write("[stage2] reliability collision guard: %d trampolines vs "
                         "%d sites -- 0 collisions\n" % (n, len(rel_ranges)))

    # --- apply: 64 entry patches ONLY (no size-field bumps). --------------------
    for rec in entries:
        foff = int(rec["file_off"], 16)
        slot = int(rec["slot_vaddr"], 16)
        struct.pack_into("<III", e.d, foff,
                         ARM_LDR_R12_PC0, ARM_LDR_PC_R12, slot)
        rec["new12"] = e.d[foff:foff + TRAMP_BYTES].hex()

    # --- re-confirm the reliability bytes SURVIVED the 64 trampoline writes. -----
    if rel_ranges:
        base_orig = open(a.pgphoto, "rb").read()
        for (ro, rl) in rel_ranges:
            if bytes(e.d[ro:ro+rl]) != base_orig[ro:ro+rl]:
                die("post-patch reliability site [%#x,%#x) was clobbered -- must "
                    "never happen; aborting" % (ro, ro + rl))

    open(out_bin, "wb").write(e.d)
    os.chmod(out_bin, 0o755)
    out_md5 = hashlib.md5(e.d).hexdigest()

    # --- emit the loader's slot table (stage2_ondisk_table.h) + manifest. -------
    th = os.path.join(a.outdir, "stage2_ondisk_table.h")
    with open(th, "w") as f:
        f.write("/* GENERATED by stage2_ondisk_patch.py -- do not edit. */\n")
        f.write("#include <stdint.h>\n#include <stddef.h>\n")
        f.write("/* 64 pointer slots at FIXED vaddrs (non-PIE EXEC).  Session 18:\n"
                " * the slots are NO LONGER in the executable's .bss (the device's\n"
                " * kernel did not reliably map the extended-.bss region writable).\n"
                " * They live in a FRESH anonymous page the loader mmaps MAP_FIXED at\n"
                " * STAGE2_SLOT_BASE as its first action -- a guaranteed-writable RW\n"
                " * page.  The loader then writes each slot with plain stores: still\n"
                " * NO mprotect, NO .text write, NO /proc/self/mem at runtime. */\n")
        f.write("#define STAGE2_SLOT_BASE 0x%08xu\n" % slot_base)
        f.write("static const struct { const char *name; uintptr_t slot; } "
                "STAGE2_SLOTS[] = {\n")
        for rec in entries:
            f.write('    {"%s", %su},\n' % (rec["symbol"], rec["slot_vaddr"]))
        f.write("};\n#define STAGE2_SLOTS_LEN "
                "(sizeof(STAGE2_SLOTS)/sizeof(STAGE2_SLOTS[0]))\n")

    manifest = {
        "input": a.pgphoto, "in_md5": in_md5,
        "output": out_bin, "out_md5": out_md5,
        "ok": True, "boundary": n, "blockers": 0,
        "entry_point": "0x%08x" % e.e_entry,          # unchanged
        "slot_base": "0x%08x" % slot_base,
        "slot_region_bytes": slot_region,
        "slot_end": "0x%08x" % slot_end,
        "slot_mechanism": "loader mmap MAP_FIXED anonymous RW page at slot_base; "
                          "NO .bss extension, NO p_memsz/sh_size change -- the "
                          "only file edits are the 64 entry trampolines",
        "segment_sizes_changed": False,
        "base_is_reliability_patched": bool(a.reliability_base),
        "reliability": rel_report,
        "tramp_encoding": ["e59fc000 ldr r12,[pc,#0]",
                           "e59cf000 ldr pc,[r12]", "<slot_vaddr>"],
        "entries": entries,
    }
    json.dump(manifest,
              open(os.path.join(a.outdir, "stage2_ondisk_manifest.json"), "w"),
              indent=2)

    print("boundary entries   : %d" % n)
    print("all resolved & >=12: yes (0 blockers)")
    print("slot base vaddr    : 0x%08x  (64 * 4 = %d bytes -> 0x%08x)"
          % (slot_base, slot_region, slot_end))
    print("slot mechanism     : loader mmap MAP_FIXED anon RW page (no .bss ext)")
    print("segment sizes      : UNCHANGED (no p_memsz / .bss sh_size bump)")
    print("entry point        : 0x%08x (unchanged)" % e.e_entry)
    print("reliability guard  : %s" % ("%d site-run(s), 0 collisions"
          % len(rel_ranges) if rel_ranges else "SKIPPED (no --reliability-base)"))
    print("in  md5            : %s%s" % (in_md5,
          "  (== known reliability-patched base)" if in_md5 == KNOWN_BASE_MD5 else ""))
    print("out md5            : %s%s" % (out_md5,
          "  (== hardware-validated stage2 binary)"
          if out_md5 == KNOWN_STAGE2_OUT_MD5 else ""))
    print("wrote %s" % out_bin)
    print("wrote %s and stage2_ondisk_manifest.json" % th)
    return 0


if __name__ == "__main__":
    sys.exit(main())
