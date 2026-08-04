# What has (and has not) been tested

## Target firmware — the ONLY one tested

| | |
|---|---|
| Device | Benro Polaris (HiSilicon Hi3559V200, ARMv7, Linux 4.9.37, glibc 2.24) |
| `FwVer` | **4.0.0.32** (date 2025.05.09) |
| Camera | **Canon EOS R5 Mark II** (USB `04a9:3314`) — the only camera tested |
| libgphoto2 built | **2.5.34** |

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

## ❌ NOT tested

- **Any camera other than the Canon R5 Mark II.** May regress other models —
  the EOS `camera_exit`/keep-device-on changes affect all Canon EOS bodies.
- **Any firmware other than FwVer 4.0.0.32.**
- Long-term stability, thermals, gimbal interaction, edge cases (disconnect
  mid-capture, etc.).
