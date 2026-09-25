#!/usr/bin/env python3
"""
Analyse the stock Benro `pgphoto` binary and (optionally) apply the minimal set
of patches that make a newer libgphoto2 `ptp2` driver work reliably with the
Canon EOS R5 Mark II (and collapse the cold-start / first-shoot delay).

Everything is discovered from the binary itself (pgphoto ships with a symbol
table), so the tool fails safe on firmware it was not built for.

Patches applied by --apply:

  1. GATE x3 — the three `strstr(name,"ptp"/"ptp2"/"PTP")` static-dispatch guards
     in libgphoto2. Each is `mov r3, r0` (e1a03000); forcing r3=0 (`mov r3,#0`,
     e3a03000) makes the guard fall through to the standard dlopen path so the
     external ptp2.so is used instead of pgphoto's compiled-in 2.5.27 copy.

  2. resetUsb() -> return 0 immediately (`mov r0,#0; bx lr`). Stock resetUsb does
     `ioctl(fd, USBDEVFS_RESET)`, which sp_Gphoto_Init calls on every init
     timeout. On a cold camera that RE-ENUMERATES the camera, restarting its
     readiness clock and causing a multi-second/-minute reset storm. Neutralising
     it lets the camera settle on its first enumeration. (Return value is ignored
     by callers.)

  3. cameraInit's ARG_LIST_FILES dispatch -> NOP. Stock cameraInit eagerly
     enumerates the ENTIRE camera filesystem over PTP at connect. On a full card
     (e.g. 8K video) that blocks the camera as "busy" for minutes before live
     view / capture can start. Skipping it makes the camera ready in seconds; the
     app still lists files on demand when the gallery is opened.

TRAMPOLINE_ADDR (pgphoto's own gp_filesystem_set_info_dirty) is also reported;
the rebuilt driver trampolines there because the device's on-disk
libgphoto2.so.6 predates that symbol.

Usage:
    analyze_pgphoto.py <pgphoto>              # print plan
    analyze_pgphoto.py <pgphoto> --apply OUT  # write patched copy to OUT
"""
import subprocess, sys, os, struct, re
from elftools.elf.elffile import ELFFile

MOV_R3_R0 = 0xE1A03000
MOV_R3_0  = 0xE3A03000
PUSH_FP_LR = 0xE92D4800        # `push {fp, lr}` — stock resetUsb prologue
MOV_R0_0   = 0xE3A00000
BX_LR      = 0xE12FFF1E
NOP        = 0xE1A00000        # `mov r0, r0`
OBJDUMP   = os.environ.get("OBJDUMP", "arm-linux-gnueabi-objdump")

def sym_value(elf, name):
    for sec in elf.iter_sections():
        if sec.header['sh_type'] not in ('SHT_SYMTAB', 'SHT_DYNSYM'):
            continue
        for s in sec.iter_symbols():
            if s.name == name and s.entry['st_shndx'] != 'SHN_UNDEF':
                return s.entry['st_value']
    return None

def sym_range(elf, name):
    """Return (start, end) vaddr range of a defined function symbol."""
    for sec in elf.iter_sections():
        if sec.header['sh_type'] not in ('SHT_SYMTAB', 'SHT_DYNSYM'):
            continue
        for s in sec.iter_symbols():
            if s.name == name and s.entry['st_shndx'] != 'SHN_UNDEF':
                v = s.entry['st_value']
                return (v, v + s.entry['st_size'])
    return None

# The static ptp2-dispatch guards live ONLY in these two libgphoto2 functions.
# Restricting discovery to their address ranges avoids matching the (very common)
# `strstr; mov r3,r0; cmp r3,#0` idiom elsewhere in the binary.
GATE_FUNCS = ("unlocked_gp_abilities_list_load_dir", "gp_camera_init")

def vaddr_to_off(elf, va):
    for s in elf.iter_sections():
        h = s.header
        if h['sh_addr'] and h['sh_addr'] <= va < h['sh_addr'] + h['sh_size'] \
           and h['sh_type'] != 'SHT_NOBITS':
            return h['sh_offset'] + (va - h['sh_addr'])
    return None

def disasm(path):
    out = subprocess.check_output(
        [OBJDUMP, "-d", "--no-show-raw-insn", path],
        stderr=subprocess.DEVNULL).decode("utf-8", "replace").splitlines()
    rx = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
    insns = []
    for L in out:
        m = rx.match(L)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip()))
    return insns

def find_gates(insns, ranges):
    """Return vaddrs of the `mov r3,r0` guard instructions that (a) follow a
    `bl strstr`, (b) precede a `cmp r3,#0`, and (c) live inside one of the
    given (start,end) function ranges."""
    def in_range(va):
        return any(a <= va < b for (a, b) in ranges)
    gates = []
    for i, (_, txt) in enumerate(insns):
        if txt.startswith("bl") and "strstr" in txt:
            for j in range(i + 1, min(i + 4, len(insns))):
                va, t = insns[j]
                if re.match(r'mov\s+r3,\s*r0$', t):
                    if in_range(va) and j + 1 < len(insns) \
                       and re.match(r'cmp\s+r3,\s*#0', insns[j + 1][1]):
                        gates.append(va)
                    break
    return list(dict.fromkeys(gates))

def find_listfiles_bl(insns, crange):
    """Inside cameraInit, find the `bl cb_arg_run` whose opt.val was just set to
    ARG_LIST_FILES (0x2c). That dispatch is the full-card file enumeration."""
    a, b = crange
    hits = []
    for k, (va, txt) in enumerate(insns):
        if not (a <= va < b):
            continue
        if txt.startswith("bl") and "cb_arg_run" in txt:
            for j in range(k - 1, max(k - 14, -1), -1):
                t = insns[j][1]
                if insns[j][1].startswith("bl") and "cb_arg_run" in insns[j][1]:
                    break  # previous action boundary
                if re.match(r'mov\s+r\d+,\s*#(44|0x2c)\b', t):
                    hits.append(va)
                    break
    return list(dict.fromkeys(hits))

def main():
    path = sys.argv[1]
    apply_to = None
    if len(sys.argv) >= 4 and sys.argv[2] == "--apply":
        apply_to = sys.argv[3]

    with open(path, "rb") as f:
        elf = ELFFile(f)
        tramp    = sym_value(elf, "gp_filesystem_set_info_dirty")
        resetusb = sym_value(elf, "resetUsb")
        crange   = sym_range(elf, "cameraInit")
        ranges = [r for r in (sym_range(elf, n) for n in GATE_FUNCS) if r]
        insns  = disasm(path)
        gates  = find_gates(insns, ranges) if ranges else []
        lfbls  = find_listfiles_bl(insns, crange) if crange else []
        data   = bytearray(open(path, "rb").read())

        # verify every gate really holds `mov r3,r0`
        good = []
        for va in gates:
            off = vaddr_to_off(elf, va)
            if off is not None and struct.unpack_from("<I", data, off)[0] == MOV_R3_R0:
                good.append((va, off))

        # verify resetUsb prologue and list-files bl encoding
        reset_off = vaddr_to_off(elf, resetusb) if resetusb is not None else None
        reset_ok = reset_off is not None and \
            struct.unpack_from("<I", data, reset_off)[0] == PUSH_FP_LR
        lf = []
        for va in lfbls:
            off = vaddr_to_off(elf, va)
            if off is not None and (struct.unpack_from("<I", data, off)[0] >> 24) == 0xEB:
                lf.append((va, off))

    if tramp is not None:
        print("TRAMPOLINE_ADDR=0x%08x" % tramp)
    for va, off in good:
        print("GATE=0x%08x" % va)
    if reset_ok:
        print("RESETUSB_ADDR=0x%08x" % resetusb)
    for va, off in lf:
        print("LISTFILES_BL=0x%08x" % va)

    if apply_to:
        problems = []
        if tramp is None:      problems.append("no trampoline symbol")
        if len(good) != 3:     problems.append("gates=%d (want 3)" % len(good))
        if not reset_ok:       problems.append("resetUsb prologue not found")
        if len(lf) != 1:       problems.append("list-files dispatch=%d (want 1)" % len(lf))
        if problems:
            sys.stderr.write("refusing to patch: " + "; ".join(problems) + "\n")
            sys.exit(2)
        for va, off in good:                       # 3 dispatch gates
            struct.pack_into("<I", data, off, MOV_R3_0)
        # resetUsb: return 0 without performing USBDEVFS_RESET.
        #
        # KEEP_RESET_USB=1 leaves the ORIGINAL prologue, i.e. the device still
        # resets the USB bus on connect. This patch exists to stop a
        # re-enumeration storm that made a cold connect take ~3 minutes -- but
        # skipping the reset is also the remaining suspect in the separate
        # cold-start fault, where gp_camera_init times out on PTP for minutes
        # after a replug. The switch exists so the two can be separated by
        # experiment rather than argument.
        if os.environ.get("KEEP_RESET_USB") == "1":
            sys.stderr.write("[analyze_pgphoto] KEEP_RESET_USB=1: USB reset left in place\n")
        else:
            struct.pack_into("<I", data, reset_off,   MOV_R0_0)   # resetUsb: return 0
            struct.pack_into("<I", data, reset_off+4, BX_LR)
        struct.pack_into("<I", data, lf[0][1],    NOP)        # skip ARG_LIST_FILES
        with open(apply_to, "wb") as g:
            g.write(data)
        os.chmod(apply_to, 0o755)

if __name__ == "__main__":
    main()
