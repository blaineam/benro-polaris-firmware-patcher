# How it works

## The device

Benro Polaris camera board: **HiSilicon Hi3559V200**, 32-bit ARM (ARMv7
Cortex-A7), Linux 4.9.37, **glibc 2.24**, GCC 6.3.0 (`arm-himix200-linux`),
soft-float EABI. Firmware `FwVer 4.0.0.32`.

NAND layout (`mtdparts`): `u-boot.bin`, `factoryParam`, `userParam`, `uImage`,
`rootfs.ubifs` (40M), `appfs.ubifs` (81M). Camera control lives in the **appfs**
(`/app`).

## Why swapping the `.so` alone does nothing

Camera control is `/app/bin/pgphoto` (supervised by `polestar_app`). It does
**not** dynamically load the on-disk `ptp2.so`; it has libgphoto2's core **and**
the `ptp2` camlib **statically compiled in** (a Benro fork of ~2.5.27). Benro
patched the loader so that when the camlib filename matches `ptp2`/`ptp`/`PTP`
it calls the *static* `camera_id`/`camera_abilities`/`camera_init` instead of
`dlopen`-ing the file:

```c
// gp_abilities_list_load_dir
if (strstr(name, "ptp2")) { camera_id(); camera_abilities(); }   // static
else { lt_dlopenext(...); lt_dlsym(...); }                        // normal path

// gp_camera_init
if (strstr(lib,"ptp") || strstr(lib,"PTP")) init = &camera_init;  // static
else init = lt_dlsym(handle,"camera_init");
```

So the on-disk `/app/lib/libgphoto2/2.5.27.1/ptp2.so` is a **filename marker
only** — its bytes never execute. Replacing it changes nothing.

## The fix

Two coordinated pieces, shipped together: a rebuilt driver, and a **14-byte**
`pgphoto` patch (three dispatch gates + two reliability edits). All reversible.

### 1. Rebuild `ptp2.so`

From a chosen libgphoto2 release, matching the device ABI exactly (soft-float
EABI, glibc-2.24 ceiling), linked against the device's own `libexif`/`libltdl`
sonames. Built **without** libxml2/jpeg/curl (the stock `pgphoto` links none of
them; Canon USB capture needs none), so the driver depends only on libraries the
device provably has. Two EOS-init behaviours that 2.5.34 added are dropped for
Polaris reliability (see **Cold-start reliability** below): the
`camera_keep_device_on` heartbeat and the `SetRemoteMode` toggle in
`camera_exit`.

### 2. Route dispatch to the rebuilt driver — 3 gates

Three `mov r3,r0` (0xe1a03000) → `mov r3,#0` (0xe3a03000) edits — one in
`gp_abilities_list_load_dir`, two in `gp_camera_init`. Each forces the `strstr`
guard's result to 0, so dispatch falls through to the standard
`lt_dlopenext`/`lt_dlsym` path and loads the rebuilt driver instead of the
compiled-in 2.5.27 copy.

### 3. Cold-start reliability — 2 more `pgphoto` edits

The rebuilt 2.5.34 driver fixes R5 II *capture*, but on the stock binary two
Benro behaviours made a cold camera take **minutes** to become usable. Both are
neutralised:

- **`resetUsb()` → `return 0`** (`mov r0,#0; bx lr`, 7 bytes). Stock `resetUsb`
  issues `ioctl(fd, USBDEVFS_RESET)`, and `sp_Gphoto_Init` calls it on every
  init timeout. On a cold camera that **re-enumerates** the camera (its USB
  device number walks 11→12→13…), restarting the camera's "endpoint ready"
  clock — a reset storm that never catches the camera. Neutralising it lets the
  camera settle on its first enumeration. (A longer OpenSession timeout was
  tried instead and rejected: it pushes `camera_init` past `polestar_app`'s ~5s
  watchdog, which then kills and respawns `pgphoto` in a crash-loop.)

- **Skip `ARG_LIST_FILES` in `cameraInit`** (one `bl cb_arg_run` → NOP, 4 bytes).
  Stock `cameraInit` eagerly enumerates the **entire camera filesystem** over PTP
  at connect. On a full card (e.g. 8K video, hundreds of GB) that recursive scan
  takes **minutes**, and `cameraInit` holds the camera "busy" the whole time, so
  live view and shutter can't start. Live view/capture never needs it — the app
  lists files on demand when the gallery is opened — so skipping it makes the
  camera ready in **seconds**.

Both extra sites are discovered from `pgphoto`'s symbol table (`resetUsb`, and
the `cb_arg_run` call inside `cameraInit` preceded by the `ARG_LIST_FILES`
opcode), so the tool still fails safe on firmware it wasn't built for.

`pgphoto` keeps its static libgphoto2 **core** (2.5.27). The rebuilt driver
binds its ~65 core calls to that core — safe because the camlib↔core API is
ABI-stable across all of 2.5.x (soname `.6`). This is verified at build time:
the tool aborts if any imported core symbol is missing from the device core.

### The one missing symbol

libgphoto2 ≥ ~2.5.28 calls `gp_filesystem_set_info_dirty()` (a trivial
cache-invalidation: `xfile->info_dirty = 1`). The device's on-disk
`libgphoto2.so.6` predates it — **but pgphoto's static core defines it** (at a
fixed address, because `pgphoto` is a non-PIE `ET_EXEC`). The tool discovers
that address from pgphoto's symbol table and compiles a tiny **trampoline** into
the new `ptp2.so` so the symbol resolves to pgphoto's own implementation,
operating on the running core's own filesystem structures. Zero struct-layout
risk. (See `container/polaris_shim.c.in`.)

## Repacking

`appfs` is extracted with `ubireader -k` (preserving uid/gid/mode/symlinks),
the two files are swapped in with their original ownership/mode, and it is
repacked with `mkfs.ubifs` + `ubinize` using geometry **read from the stock
image** (min_io 2048, LEB 126976, max_leb 660, fanout 8, LZO, PEB 128K,
image_seq preserved). `firmwareInfo` is regenerated so the device's on-board
MD5 check passes.

## Why a bad SD image is normally recoverable

`polestar_app` (Linux userspace) **never writes NAND**. It verifies MD5s, then
reboots; **U-Boot** performs the flash from the SD card on the next boot. This
tool modifies neither U-Boot nor the U-Boot environment (`camera/config`), so
the flasher itself is never overwritten — a bad/incomplete `appfs` image is
normally fixed by re-flashing (stock or corrected). **This is an argument for
recoverability, not a guarantee; flash at your own risk.**

## Trust boundaries / safety checks the tool enforces

- Aborts unless it finds **exactly 3** dispatch gates holding the expected
  bytes, the trampoline symbol, the **`resetUsb`** symbol with its expected
  prologue, and **exactly 1** `ARG_LIST_FILES` dispatch in `cameraInit` — so it
  won't blindly patch unknown builds.
- Aborts if the rebuilt driver's **glibc ceiling > 2.24**.
- Aborts if the driver imports a **core symbol the device lacks**.
- Aborts if the `pgphoto` patch touches anything other than **14 bytes**
  (3 gates + `resetUsb` return-0 + `ARG_LIST_FILES` skip).
- Leaves kernel, rootfs, gimbal blobs, and U-Boot env byte-identical.
