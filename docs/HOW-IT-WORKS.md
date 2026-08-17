# How it works

## The device

Benro Polaris camera board: **HiSilicon Hi3559V200**, 32-bit ARM (ARMv7
Cortex-A7), Linux 4.9.37, **glibc 2.24**, GCC 6.3.0 (`arm-himix200-linux`),
soft-float EABI. Firmware `FwVer 4.0.0.32`.

NAND layout (`mtdparts`): `u-boot.bin`, `factoryParam`, `userParam`, `uImage`,
`rootfs.ubifs` (40M), `appfs.ubifs` (81M). Camera control lives in the **appfs**
(`/app`).

---

# Full mode (the default): replacing the WHOLE libgphoto2 stack

`pgphoto` has libgphoto2's **core + port + ptp2 camlib** statically compiled in
(a Benro fork of ~2.5.27). ptp2-only mode (below) swaps just the on-disk camlib
and keeps that 2.5.27 core. **Full mode replaces the entire stack** — core, port,
ptp2, and usb1 all become fresh **2.5.34** — without recompiling `pgphoto` and
without any runtime code-page patching. It is the mode
[verified on real hardware](TESTED.md).

## The mechanism: on-disk trampolines into a dlopen'd fresh core

`pgphoto` is a non-PIE `ET_EXEC`, so every internal libgphoto2 entry point it
calls sits at a **fixed address**. Full mode intersects the API `pgphoto` calls
with the public LGPL libgphoto2 symbols and finds **64 boundary functions**
(`gp_*` ×55 + `gp_port_*` ×9). (`gp_params_*`, which are gphoto2-CLI code Benro
compiled in and extended, are deliberately *not* touched.) Each of the 64 is
redirected into a freshly-built 2.5.34 core:

1. **On-disk patch (in the file, once).** `stage2_patch.py` overwrites the first
   12 bytes of each boundary function with an absolute indirect jump through a
   per-function pointer *slot*:

   ```
   entry+0:  ldr r12, [pc, #0]   ; r12 = *(entry+8) = &slot_i
   entry+4:  ldr pc,  [r12]      ; pc = *slot_i   (the fresh-core target)
   entry+8:  .word &slot_i       ; a CONSTANT slot address, known at patch time
   ```

   The smallest boundary function is 56 bytes, so 12 fits with wide margin. The
   binary stays **byte-count-identical to stock** — no segment/size bumps, entry
   point unchanged; only 719 bytes inside `.text` differ.

2. **The slot page (a fresh mmap, not `.bss`).** The 64 slots live at a fixed base
   `0x30000000`, in the wide free gap between `pgphoto`'s image+heap and the shared
   libraries. The loader's **very first action** is to `mmap` a fresh anonymous
   `PROT_READ|PROT_WRITE` page `MAP_FIXED` there — after checking `/proc/self/maps`
   and *refusing* (never clobbering) if anything already occupies it. (An earlier
   design carved the slots out of an extended `.bss`; the device's Hi3559V200
   kernel did not reliably map that region writable, so the loader owns the page
   now.)

3. **The loader fills the slots (`libpolaris_stage2.so`).** LD_PRELOAD'd (test
   bundle) or `DT_NEEDED`/wrapper (firmware). Its constructor: map the slot page →
   write `&abort_stub` into all 64 slots (fail-closed baseline, published with a
   barrier before any thread can call a redirected function) → `dlopen` the fresh
   core + port → `dlsym` each of the 64 and store the real target into its slot →
   `slots filled 64/64`. **No `mprotect`, no `.text` write, no `/proc/self/mem` at
   runtime** — the reason three earlier runtime-`.text`-patching loaders were each
   crashed or refused by this kernel. A `SIGSEGV`/`SIGBUS`/`SIGILL` pinpoint
   handler + unbuffered checkpoints make any residual fault diagnosable.

Benro code → `bl` stock `gp_*` addr → trampoline → slot → **fresh 2.5.34 core**.
The fresh core resolves its own internals through its own PLT — a clean split.

## Why the fresh core is ABI-safe with Benro's binary

Every public libgphoto2 struct `pgphoto` touches is **byte-identical between
2.5.27 and 2.5.34** (the `.6`/`.12` soname promise; verified field-by-field and by
compiled `sizeof`). The one exception is `struct _Camera`, which **Benro** (not
upstream) extended with a 4120-byte tail (→ 4140 bytes; upstream is 20). So the
fresh core is built with a one-line pad — `char _reserved_tail[4120];` — making its
`gp_camera_new()` allocate the same 4140 bytes Benro's code reads. **4120 is an
interop SIZE, not Benro code; nothing proprietary is shipped, and upstream never
reads the tail (ABI-inert).** The build fails closed unless the rebuilt core's
`gp_camera_new` is confirmed to allocate 4140.

## The reliability-patched base

The 64 trampolines only redirect the *boundary* API. `pgphoto`'s own non-boundary
cold-start code (`resetUsb`, the eager `ARG_LIST_FILES` full-card scan) is **not**
trampolined, so full mode first applies the **same 14-byte reliability patch** that
ptp2-only mode applies (symbol-discovered: `resetUsb → return 0`, skip
`ARG_LIST_FILES`, + the 3 dispatch gates), then trampolines over that base. A
fail-closed collision guard proves no trampoline overwrites a reliability byte
(the reliability sites live in non-boundary functions or mid-function, so there
are provably **0 collisions**).

## Where the fresh camlib/iolib actually load from

The wrapper exports `CAMLIBS=/app/lib/stage2/libgphoto2/2.5.34`, but the swapped
2.5.34 core does **not** end up loading `ptp2.so` from there. Device tracing shows
it resolves its camlib from the **stock on-disk layout** —
`/app/lib/libgphoto2/2.5.27.1/ptp2.so` — and the port loader `dlopen`s `usb1.so`
from `/app/lib/libgphoto2_port/0.12.0/usb1.so`. So full mode writes the fresh
`ptp2.so`/`usb1.so` to **both** places: the `stage2/…/2.5.34` + `…/0.12.2` dirs
**and** the stock `2.5.27.1` + `0.12.0` paths (stock perms preserved — the stock
camlib is `0750`). If only the `stage2` copies existed, the core would silently
bind the **stale stock 2.5.27 ptp2** and the Canon driver would never come up.

## The two shims: showing a card, and tethered capture

The loader `libpolaris_stage2.so` carries three small env-gated shims. They read
and write only **public** libgphoto2 struct fields and call only **public**
`gp_camera_set_config` / `gp_camera_set_single_config` APIs — no firmware code,
no new exported symbols. The wrapper turns both on.

- **Shim #1 — storage / "no card"** (`STAGE2_STORAGE_SHIM=1`). Benro's app decides
  whether to show a memory card from the camera's reported storage info. Driving
  the R5 II tethered, the shim writes the Benro `_Camera` storage-type field
  (`storageType`, at offset `+0x1c` in Benro's extended `_Camera`) to `2` so the
  app sees a card and raises **no "no card" warning**.
- **Shims #2 + #3 — force Internal-RAM capture** (`STAGE2_TETHER_CAPTURE=1`).
  Shim #2 wraps `gp_camera_set_config` and tree-walks the widget set;
  shim #3 wraps `gp_camera_set_single_config`. Both force Canon's `capturetarget`
  to **"Internal RAM"**. The Polaris sets configs one at a time via
  `set_single_config`, so **shim #3 is the one that actually fires** in practice;
  #2 is there for completeness (any caller that pushes the whole config tree).

### Why tethered (Internal-RAM) capture

With a card selected as the capture target, the R5 II's post-capture
`ObjectAddedEx` PTP event is **not delivered** through the fresh 2.5.34 core in
the Polaris' event loop, so the shot fires but the "file is ready" signal never
arrives and the download hangs (the original stock-firmware symptom, in a new
guise). Capturing to **Internal RAM** instead takes Canon's `ObjectTransfer`
path, which **does** fire — so the capture completes and **both the JPEG and the
RAW** transfer down to the Polaris.

### EOS-init error tolerance (the LGPL `dbg_patch`)

Running ptp2 against a *fresh* 2.5.34 core also re-exposes Canon's EOS init-time
event/keep-alive drains exactly as upstream wrote them. On the R5 II a couple of
those `C_PTP(ptp_check_eos_events(...))` / `CR(camera_keep_device_on(...))` calls
return a transient non-OK the **first** time and abort `camera_init`, dropping the
camera to the generic "USB PTP Class Camera" driver (no settings, no live view).
`container/dbg_patch.py` (POLARIS_DBG) makes exactly those drains non-fatal —
`config.c` `check_eos_events` ×13, `library.c` `keep_device_on` ×3 +
`check_eos_events` ×1 — by capturing and discarding the transient result instead
of returning it. It is a mechanical edit of upstream's own LGPL source (a
documented modification, see [NOTICE](../NOTICE)), adds no symbols or
dependencies, and is a no-op once the camera answers OK. It is **not** the
throwaway `trace_patch.py` instrumentation (that is dev-only, `TRACE=1`, never
shipped).

## The self-driving wrapper (no env vars from the launcher)

`polestar_app` launches `/app/bin/pgphoto` with no special environment, so full
mode installs a tiny 9-line `sh` wrapper there (no logging) that exports
`CAMLIBS`/`IOLIBS` (→ the stage2 dirs), `LD_LIBRARY_PATH=/app/lib/stage2:/app/lib`,
`LD_PRELOAD=/app/lib/stage2/libpolaris_stage2.so`, and the two shim toggles
`STAGE2_STORAGE_SHIM=1` + `STAGE2_TETHER_CAPTURE=1`, then `exec`s
`/app/lib/stage2/pgphoto.stage2ondisk`. Everything the fresh stack needs lives
under `/app/lib/stage2`; its runtime deps (`libltdl.so.7`, `libexif.so.12`,
`libusb-1.0.so.0`, `libudev.so.1`) are already present on the device.

## Reversible on-device testing (before you flash)

Full mode also emits `out/stage2-ondisk/`, a bundle you can copy to the camera and
`sh ondisk/install_stage2.sh` — it backs up the real `/app/bin/pgphoto` to
`/app/sd/pgphoto.prestage2.bak` (only once), populates `/app/lib/stage2`, and
installs the wrapper. `sh ondisk/restore_stock.sh` copies the stock binary back.
This lets you confirm `slots filled 64/64` → `gp_camera_init 0` → a capture on your
own unit **before** committing a flash.

## Determinism

The entire build is byte-for-byte reproducible from the same inputs. Running the
public patcher in its container against a stock FwPkt (default full mode)
reproduces **every** hardware-validated component exactly: the reliability base,
the trampolined `pgphoto.stage2ondisk` (`a83ac7bb`), the loader
`libpolaris_stage2.so` (`74f681de`), the wrapper (`868c3097`), and all four rebuilt
LGPL libraries — core `b4c7ec31`, port `aa3ff350`, ptp2 `9bdbd13d`, usb1
`5199e973`. The only value that shifts run-to-run is the **whole-image**
`appfs.ubifs` md5, because UBIFS stamps a per-inode mtime at repack time; every
file *inside* the image is byte-identical. See [TESTED.md](TESTED.md) for the
full component table.

---

# ptp2-only mode (the conservative fallback)

The rest of this document describes `--ptp2-only`, which keeps `pgphoto`'s
compiled-in 2.5.27 core and swaps only the on-disk ptp2 camlib + usb1 iolib.

## Why swapping the camlib `.so` alone does nothing

(This applies to the **camlib** — `ptp2.so`. The **port iolib** — `usb1.so` — is
different: `pgphoto` `dlopen`s it, so replacing it *does* take effect. See
*Also swapping the `usb1` iolib* below.)

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

## Also swapping the `usb1` iolib (USB transport)

The `ptp2` camlib is only the *driver*; the bytes actually move over USB through
libgphoto2's **port** layer and its `usb1` iolib
(`/app/lib/libgphoto2_port/0.12.0/usb1.so`). Updating that too is the first step
toward replacing the whole stack, not just the camlib.

### The port layer is `dlopen`ed — unlike the camlib

The camlib needs three `pgphoto` gates because Benro compiled `ptp2` **statically**
into `pgphoto` and short-circuits its dispatch (`strstr(name,"ptp2")` → static
`camera_init`), so the on-disk `ptp2.so` is a dead marker until the gates re-route
to `lt_dlopenext`. The **port layer has no such short-circuit.** In the stock
binary:

```c
// gp_port_info_list_load  → lt_dlforeachfile(IOLIBS dir, foreach_func)
// foreach_func            → lt_dlopenext(iolib); lt_dlsym("gp_port_library_type"/"_list")
// gp_port_set_info        → lt_dlopenext(iolib); lt_dlsym("gp_port_library_operations")
```

`pgphoto` unconditionally `dlopen`s every iolib in the directory and binds the
selected one's `gp_port_library_operations`. (Confirmed by disassembly: those
call sites carry no `strstr`/`strcmp` guard, and `pgphoto` does **not** define
`gp_port_library_type/list/operations` itself — they come from the loaded `.so`.)
So the stock `usb1.so` is **live code**, and simply replacing the file takes
effect — **no `pgphoto` edit is needed for the iolib.**

### It's libusb-based, and the device has libusb

The stock `usb1.so` is an ordinary libgphoto2 `usb1` iolib built against
**libusb-1.0**: its `DT_NEEDED` is `libgphoto2_port.so.12`, `libusb-1.0.so.0`,
`libpthread.so.0`, `libc.so.6`, and it imports the `libusb_*` API. The device
ships `libusb-1.0.so.0` in `/app/lib`, which is what `pgphoto`'s loader resolves
at `dlopen` time.

> A note on a red herring: `pgphoto` *itself* has **zero** `libusb` symbols and no
> `libusb`/`udev` in its own `DT_NEEDED` — it reaches for raw `USBDEVFS` ioctls
> only for Benro's own `resetUsb`/`scanUsb` helpers. That is *not* evidence that
> the transport is raw-usbfs: `libusb` is a dependency of the **`dlopen`ed**
> `usb1.so`, not of `pgphoto`, so it never appears in `pgphoto`'s symbol table.
> The rebuilt `usb1.so` therefore matches stock: **libusb-based, not raw usbfs.**

### The rebuild

From the **same** libgphoto2 release as the camlib, the tool builds
`libgphoto2_port`'s `usb1` iolib (`--with-libusb-1.0`), ABI-matched (soft-float
EABI, glibc-2.24 ceiling) and linked against the **device's own**
`libusb-1.0.so.0` soname (staged from the appfs), so it binds the exact libusb
the device runs. libtool over-links a spurious `libltdl.so.7` into the iolib
(via the port library's dependency chain); since `usb1.so` references no `lt_dl*`
symbol, the tool drops that `DT_NEEDED` entry so the result matches stock's
dependency set **exactly**.

The device's `libusb-1.0.so.0` transitively needs `libudev.so.1` (in the rootfs).
That only matters for `configure`'s `libusb_init` link-test, which
`-Wl,--allow-shlib-undefined` satisfies; `usb1.so` never references a udev symbol,
so **udev does not enter `usb1.so`'s `DT_NEEDED`** — verified against stock.

### Fail-safe verification (aborts on any mismatch)

Before the swap the tool requires the rebuilt `usb1.so` to be soft-float EABI with
a glibc ceiling ≤ 2.24, to export `gp_port_library_type`/`_list`/`_operations`, to
have a `DT_NEEDED` that is a **subset** of the stock `usb1.so`'s (no new shared
library), to import only core/port symbols the device's `libgphoto2_port.so.12` /
`libgphoto2.so.6` provide, and to import only `libusb_*` symbols the device's own
`libusb-1.0.so.0` defines. Any failure aborts before anything is written.

### ABI note (2.5.34 iolib ↔ 2.5.27 port core)

Like the camlib, the rebuilt iolib is a newer libgphoto2 talking to `pgphoto`'s
compiled-in 2.5.27 **port core** through the `GPPortOperations` table and the
`gp_port_*` API. That interface is ABI-stable across all of 2.5.x (iolib version
`0.12.x`, soname `.12`), and the symbol-resolution check above proves every call
binds — the same guarantee that makes the camlib swap safe.

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
- For the **usb1 iolib** (when enabled): aborts unless it is soft-float EABI with
  glibc ceiling ≤ 2.24, exports the three iolib entry points, its `DT_NEEDED` ⊆
  the stock `usb1.so`'s, and every core/port and `libusb_*` symbol it imports is
  provided by the device's port core / own `libusb-1.0.so.0`.
- Leaves kernel, rootfs, gimbal blobs, and U-Boot env byte-identical.
- Touches **at most three** appfs files (`ptp2.so`, `usb1.so`, `pgphoto`); every
  other appfs file — including all other iolibs (`disk`/`serial`/`ptpip`/… ) — is
  byte-identical to stock.
- With `--ssh-key` it *adds* exactly one new appfs file (a boot hook the stock
  `bootapp` already looks for) and still **modifies** none. See below.

---

# Optional: SSH debug access (`--ssh-key`)

## The device already runs sshd

The stock rootfs ships **OpenSSH 7.8p1** at `/usr/local/bin/sshd` (plus `scp` and
`sftp`), and `/etc/init.d/rcS` starts it on every boot as its last action:

```sh
umount /dev/pts; rm -rf /dev/pts; mkdir /dev/pts; mount /dev/pts
/usr/local/bin/sshd
```

Its `sshd_config` is permissive by Benro's choice — `PermitRootLogin yes`,
`PasswordAuthentication yes`, `AuthorizedKeysFile .ssh/authorized_keys` — and
`/etc/passwd` gives `root` a real (DES-crypt) password and `/bin/sh`. So SSH is
**already listening on a stock device**; the only thing missing for key login is
a key in `/root/.ssh`, which lives in the **rootfs** — the partition this tool
ships byte-identical.

## Why no firmware file has to be modified

`/app/bootapp` (run at boot by `/etc/init.d/S10mpp`, which `ubiattach`es and
mounts the appfs, then runs `./bootapp`) ends with two **optional** hooks:

```sh
# set network and start telnetd
if [ -f "/app/network_telnetd.sh" ];then cd /app; ./network_telnetd.sh; fi
# start ci agent
if [ -f "/app/start_agent.sh" ];then cd /app; ./start_agent.sh; fi
```

Neither file exists in stock firmware — they are Benro's own debug/CI escape
hatches. `--ssh-key` **creates the first free one** (0755, `bootapp`'s uid/gid).
That is the entire firmware change: one added file, nothing edited, `bootapp`
itself untouched. If a future firmware ships its own `network_telnetd.sh`, the
patcher steps to `start_agent.sh`; if it calls neither, the patcher **aborts**
rather than editing `bootapp`.

## What the hook does at boot

It appends each authorised key to `/root/.ssh/authorized_keys` — `grep -qxF`
first, so re-running it is idempotent and any key already on the device is kept —
then `chmod 700`/`600` + `chown 0:0` so sshd's `StrictModes` accepts the file.
(`/` is mounted `rw` per the U-Boot `bootargs`, so that write persists.) It runs
**before** `rcS` reaches its `sshd` line, and carries a belt-and-braces
`( sleep 20; pidof sshd || /usr/local/bin/sshd ) &` for a firmware that ever
stops starting sshd itself. It is deliberately **not** `set -e`: a boot hook must
never be able to abort the rest of `bootapp`.

## Key validation (`container/gen_ssh_hook.py`)

Each line must be a real public key: a type from the allow-list sshd 7.8 accepts
(`ssh-ed25519`, `ssh-rsa`, `rsa-sha2-*`, `ecdsa-sha2-nistp*`, the two `sk-*`
FIDO types — **not** `ssh-dss`, which that sshd rejects by default), valid
base64, and an SSH wire-format body whose embedded algorithm name matches the
type field. Private key material is detected and refused. The `SHA256:`
fingerprint of every accepted key is printed (it matches `ssh-keygen -lf`) and
baked into the hook's header comment, so what is authorised is auditable both in
the build log and on the device.

## Removing it

Delete `/app/<hook>` and drop the key from `/root/.ssh/authorized_keys`, or
reflash stock firmware — that rewrites both partitions, so both the hook and the
`authorized_keys` line go away. `out/ssh-debug/` carries the same script
standalone, for installing it on a device you can already reach without flashing
at all.
