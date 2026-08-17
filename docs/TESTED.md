# What has (and has not) been tested

## Target firmware — the ONLY one tested

| | |
|---|---|
| Device | Benro Polaris (HiSilicon Hi3559V200, ARMv7, Linux 4.9.37, glibc 2.24) |
| `FwVer` | **4.0.0.32** (date 2025.05.09) |
| Camera | **Canon EOS R5 Mark II** (USB `04a9:3314`) — the only camera tested |
| libgphoto2 built | **2.5.34** |

## Modes: what was verified on hardware

- **Full libgphoto2 (default)** — **HARDWARE-VERIFIED, flashed end-to-end.** On a
  physical Canon EOS R5 Mark II + Benro Polaris, the full-stack swap (fresh 2.5.34
  core + port + ptp2 + usb1 via the on-disk trampoline loader, with the storage +
  tethered-capture shims) was confirmed working from a **cold boot**:
  `mmap slot page → dlopen core/port → slots filled 64/64 → gp_camera_init ret 0`.
  Confirmed on the device:
  - **Settings stick** (ISO/shutter/aperture/WB/focus).
  - **Live view** streams.
  - **No "no card" warning** (storage shim).
  - **Capture downloads both JPEG and RAW** (Internal-RAM/tethered capture via the
    `ObjectTransfer` path — card-mode `ObjectAddedEx` is not delivered through the
    fresh core).
- **ptp2-only (fallback)** — also hardware-verified (this was the project's first
  working version): detection, live view, controls, capture, survives reboot.

Full mode is the default because it is the one validated end-to-end today; ptp2-only
remains as the conservative fallback (`--ptp2-only`).

## Reproducibility (full mode) — the patcher reproduces the flashed build

The public patcher, run in its own container against a stock FwPkt with **no
flags** (default full mode), reproduces the **exact** hardware-validated
components **byte-for-byte**. Every file placed in the appfs matches the flashed,
device-confirmed build:

| Component | appfs path | md5 |
|---|---|---|
| core `libgphoto2.so.6` | `lib/stage2/` | `b4c7ec318ffc2d4a2d4f72ca1055b44b` |
| port `libgphoto2_port.so.12` | `lib/stage2/` | `aa3ff3507774bcd9f7af32993b2f869b` |
| ptp2 `ptp2.so` (camlib) | `lib/stage2/libgphoto2/2.5.34/` **and** `lib/libgphoto2/2.5.27.1/` | `9bdbd13d064f7f94102926e013fdf977` |
| usb1 `usb1.so` (iolib) | `lib/stage2/libgphoto2_port/0.12.2/` **and** `lib/libgphoto2_port/0.12.0/` | `5199e973f5a0f9012f261383470ef27f` |
| loader `libpolaris_stage2.so` | `lib/stage2/` | `74f681de5a43e068df36ae61001a4e79` |
| trampolined `pgphoto.stage2ondisk` | `lib/stage2/` | `a83ac7bbee13078ca53807a452961285` |
| wrapper `pgphoto` | `bin/` | `868c3097d6337689a431da76fe45343b` |

The rebuilt **LGPL libraries** are clean upstream builds (soft-float EABI5,
glibc ≤ 2.24, all 64 boundary symbols exported, `_Camera` padded so
`gp_camera_new` allocates 4140 bytes, ptp2 carries the documented EOS-init
non-fatal edit) — and they now reproduce the validated binaries **exactly**.

> The whole-image `appfs.ubifs` md5 is **not** a fixed constant across runs:
> UBIFS stores a per-inode mtime, so files written at repack time carry the
> current wall-clock and the container md5 of the packed image shifts between
> runs. This is cosmetic — **every regular file inside the image is
> byte-identical** to the table above (verify by re-extracting with
> `ubireader_extract_files` and md5-summing the components). The mechanism is
> fully reproduced; the image is safe to flash.

Stock component MD5s this tool was validated against (from `firmwareInfo`):

```
config      1905e2d041be62b679f7dc6c64ab9d3a   (unchanged by tool)
uImage      5f6a0c1861a254371c4a956b57f26685   (unchanged)
rootfs      778b27bcade9ddc6ea4a7cb45254c551   (unchanged)
appfs       47f2ae680be3a5f5d69aa20e20a2397b   (REBUILT)
polaris403  4facafa7d29c1e6c2a125b8309c9b901   (unchanged)
polaris413  c0299d06a15f5c2fbecb9a6db76a29c5   (unchanged)
```

## `pgphoto` patch sites (FwVer 4.0.0.32)

Discovered automatically from the symbol table, but recorded here for reference.
**14 bytes total.**

Dispatch gates — each `mov r3,r0` (`e1a03000`) → `mov r3,#0` (`e3a03000`):

| Site | vaddr | function |
|---|---|---|
| gate 1 | `0x26248` | `unlocked_gp_abilities_list_load_dir` (strstr "ptp2") |
| gate 2 | `0x29d88` | `gp_camera_init` (strstr "ptp") |
| gate 3 | `0x29db8` | `gp_camera_init` (strstr "PTP") |

Reliability edits:

| Site | vaddr | change | why |
|---|---|---|---|
| `resetUsb` | `0x21830` | `push {fp,lr}` → `mov r0,#0; bx lr` | stop `USBDEVFS_RESET` re-enumeration storm on cold connect |
| list-files | `0xfffd0` | `bl cb_arg_run` → `nop` | skip full-card PTP file scan in `cameraInit` (minutes → seconds) |

Trampoline target (`gp_filesystem_set_info_dirty` in pgphoto's static core):
`0x0003fa00` (ARM, inside the `R E` load segment).

## appfs geometry (read from the stock image, reproduced on repack)

```
min_io 2048   LEB 126976   max_leb_cnt 660   fanout 8   compr lzo
PEB 131072    vid_hdr_offset 2048   data_offset 4096   image_seq 958962934
```

## `usb1` iolib swap (FwVer 4.0.0.32) — device-verified

The USB transport iolib is swapped alongside the camlib:

| | |
|---|---|
| File | `/app/lib/libgphoto2_port/0.12.0/usb1.so` |
| Loaded by | `pgphoto` at runtime via `lt_dlopenext` (**not** statically dispatched — no gate needed) |
| Stock `usb1.so` | libusb-based; `DT_NEEDED` = `libgphoto2_port.so.12`, `libusb-1.0.so.0`, `libpthread.so.0`, `libc.so.6`; glibc ceiling `GLIBC_2.4`; 85756 B |
| Rebuilt `usb1.so` | libgphoto2 2.5.34 `usb1`, `--with-libusb-1.0`, linked against the device's own `libusb-1.0.so.0`; soft-float EABI; glibc ceiling `GLIBC_2.4`; **identical `DT_NEEDED`**; ~86028 B |

Device libs the rebuilt iolib binds against (present in stock appfs `/app/lib`):
`libgphoto2_port.so.12`, `libgphoto2.so.6`, `libusb-1.0.so.0`.

## ✅ Verified (offline / in emulation)

- Rebuilt `ptp2.so` is **soft-float EABI5**, **glibc ceiling 2.4/2.7**
  (device has 2.24), sonames/`DT_NEEDED` all satisfiable on-device.
- Rebuilt `usb1.so` is **soft-float EABI5**, **glibc ceiling 2.4**, exports the
  three iolib entry points (`gp_port_library_type`/`_list`/`_operations`), its
  `DT_NEEDED` is **identical to the stock `usb1.so`** (no new shared library),
  all 10 core/port symbols it imports resolve against the device's
  `libgphoto2_port.so.12` / `libgphoto2.so.6`, and all **29** `libusb_*` symbols
  resolve against the device's own `libusb-1.0.so.0`.
- With usb1 enabled, the repacked appfs differs from stock in **exactly three**
  files (`pgphoto`, `ptp2.so`, `usb1.so`); every other iolib
  (`disk`/`serial`/`ptpip`/`usbscsi`/`usbdiskdirect`) is byte-identical.
- `ptp2.so` is byte-identical whether or not usb1 is swapped, and `--no-usb1`
  reproduces the original two-file (`pgphoto` + `ptp2.so`) change.
- **dlopen succeeds** against the device's *own* stock 2.5.27 core (qemu-arm):
  every symbol resolves; **all 2467 models register**.
- **Canon EOS R5 Mark II registers**: USB `04a9:3314`, `ops=0x39`
  (capture-image + live-preview + config + trigger-capture), USB port.
- Corrected a real **upstream typo**: 2.5.34 ships the R5 II as `EOS 5Rm2`
  (letters transposed); the tool restores `EOS R5m2`.
- `pgphoto` patch changes **exactly 14 bytes** (3 gates + `resetUsb` + list-files);
  disassembly confirms the gates take the dlopen path, `resetUsb` returns 0
  immediately, and the `ARG_LIST_FILES` dispatch is a NOP.
- Repacked `appfs.ubifs` **round-trips**: re-extraction differs from stock in
  **only** `bin/pgphoto` and `lib/.../ptp2.so`; all other files, permissions,
  owners, and symlinks are identical. UBI header (vid/data offset, image_seq)
  and the UBIFS **space_fixup** superblock flag match stock.

## ✅ Verified ON REAL HARDWARE (Canon EOS R5 Mark II + Benro Polaris)

Confirmed by the maintainer on a physical R5 Mark II:

- **Camera detection** is immediate.
- **Live view** starts immediately and streams to the app.
- **Camera controls / settings** (ISO, shutter, aperture, WB, focus, …) work.
- **Shutter / capture** works, and images download over the Polaris.
- The camera is **ready in seconds** on a cold connect (vs. a ~3-minute stall
  before the `resetUsb` + list-files edits), even with a card full of 8K video.
- Survives reboot (UBIFS space_fixup set).
- **The `usb1` iolib swap (the `port + camlib` build, `usb1` on by default)** is
  flash-verified end-to-end: detection, live view, controls, and capture all work
  and **survive a reboot**. The rebuilt 2.5.34 `usb1` (port 0.12.2) coexists fine
  with `pgphoto`'s compiled-in 0.12.0 port core in practice.

## ⚠️ `--ssh-key` — verified in the build, NOT yet flash-verified

The optional SSH hook (`--ssh-key`) has been verified **through the patcher**:
against stock FwVer 4.0.0.32 it adds exactly one appfs file
(`/app/network_telnetd.sh`, same uid/gid/mode as `bootapp`), modifies nothing
else, and the repacked appfs diffs against stock as *only* the usual mode files
plus that one addition. The facts it relies on were read out of this firmware's
own images: `/etc/init.d/rcS` starts `/usr/local/bin/sshd` (OpenSSH 7.8p1),
`sshd_config` has `PermitRootLogin yes` + `AuthorizedKeysFile .ssh/authorized_keys`,
`/app/bootapp` runs `/app/network_telnetd.sh` if it exists, and `bootargs` mounts
`/` `rw`.

**It has not been flashed and logged into on hardware.** If you try it, the
device-side script is `out/ssh-debug/` — you can install that on a device you can
already reach and confirm the login before committing to a flash.

## ❌ NOT tested

- **Any camera other than the Canon R5 Mark II.** May regress other models —
  the EOS `camera_exit`/keep-device-on changes affect all Canon EOS bodies.
- **Any firmware other than FwVer 4.0.0.32.**
- Long-term stability, thermals, gimbal interaction, edge cases (disconnect
  mid-capture, etc.).
