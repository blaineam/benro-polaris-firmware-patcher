# Benro Polaris Firmware Patcher

Patch the Benro Polaris camera firmware to control cameras its stock libgphoto2
can't. It rebuilds the Polaris' camera-control driver (`libgphoto2` / the `ptp2`
PTP camlib) from a **newer libgphoto2 release**, applies a few surgical edits to
the `pgphoto` control binary, and repackages everything into a **flashable
firmware image**.

It was created to fix the **Canon EOS R5 Mark II** on the Polaris, where three
things were broken on stock firmware (libgphoto2 2.5.27, ~2021):

1. **Image capture hung forever** — fixed by rebuilding the driver from
   libgphoto2 **2.5.34** (correct Canon object-download/event handling).
2. **A cold connect took ~3 minutes** before live view / shutter would start —
   fixed by neutralising a USB-reset re-enumeration storm (`resetUsb`) and by
   skipping the eager full-card PTP file scan `cameraInit` runs at connect.
3. Reboot-hang after flashing — avoided by preserving the UBIFS `space_fixup`
   flag on repack.

Result on a real R5 Mark II (flash-verified end-to-end): **immediate detection,
settings that stick, live view, no "no card" warning, and capture that downloads
both JPEG and RAW.** See [docs/TESTED.md](docs/TESTED.md).

## Two modes (full is the default)

| Mode | What it swaps | Flag |
|---|---|---|
| **Full libgphoto2** — **DEFAULT** | The **whole** 2.5.34 stack: **core + port + ptp2 camlib + usb1 iolib**. `pgphoto`'s compiled-in 2.5.27 core is bypassed by an on-disk *trampoline* into the fresh core. **This is the mode verified end-to-end on the R5 Mark II** (cold boot, capture, live view). | *(none)* |
| **ptp2-only** — conservative fallback | Keeps `pgphoto`'s compiled-in 2.5.27 **core**, swaps only the **ptp2 camlib + usb1 iolib** and applies a 14-byte `pgphoto` patch. Smaller change; use it if the full swap misbehaves on your setup. | `--ptp2-only` / `-Ptp2Only` |

Both modes are **reversible** by re-flashing your stock `FwPkt`. Full mode also
writes a `stage2-ondisk/` bundle so you can **test on-device before flashing**
(install/revert scripts; see [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)).

---

## ⚠️ READ THIS FIRST — DISCLAIMERS

> **USE AT YOUR OWN RISK. NO WARRANTY. THIS IS NOT A BENRO PRODUCT.**
>
> - **Only tested against Benro Polaris firmware `FwVer 4.0.0.32`** with a
>   **Canon EOS R5 Mark II**. The tool refuses to patch if it can't find the
>   exact code patterns it expects, but "it ran" is not "it's safe for you."
> - **I only own a Canon R5 Mark II.** Applying this may **change or break
>   support for other camera models.** It has not been tested with any other
>   camera. If you use a different camera, you are the first tester.
> - Firmware flashing can go wrong. While the Polaris' SD-card update is
>   performed by U-Boot (which this tool does **not** touch — see
>   [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for why a bad SD image is
>   normally recoverable by re-flashing), **no one can promise your specific
>   unit won't be affected. Proceed only if you accept the risk of bricking.**
> - **Always keep your original, unmodified `FwPkt` as the factory-restore
>   image.** Re-flashing it returns you to stock.
> - This project ships **no firmware**. You supply your own stock `FwPkt`
>   (see *Obtaining the stock firmware*). It also ships no libgphoto2 binaries —
>   they are built from source at run time.
> - Not affiliated with or endorsed by Benro, Snoppa, Canon, or the gphoto
>   project.

If any of the above is unacceptable to you, **do not use this tool.**

---

## What it actually changes

### Full mode (default) — hardware-verified end-to-end

Inside the firmware's `appfs`, full mode:

1. Replaces **`/app/bin/pgphoto`** with a tiny 9-line `sh` **wrapper** (no logging)
   that sets `CAMLIBS`/`IOLIBS`/`LD_LIBRARY_PATH`, `LD_PRELOAD`s the loader, turns
   the two runtime shims on, and `exec`s the trampolined binary.
2. Adds **`/app/lib/stage2/`** — a self-contained new stack the wrapper runs:
   - `libgphoto2.so.6` + `libgphoto2_port.so.12` — the fresh **2.5.34 core + port**;
   - `libgphoto2/2.5.34/ptp2.so` + `libgphoto2_port/0.12.2/usb1.so` — camlib + iolib;
   - `libpolaris_stage2.so` — the **loader** that redirects the 64 libgphoto2 API
     entry points `pgphoto` calls into the fresh core (an **on-disk trampoline**,
     no runtime code patching). It also carries three small env-gated **shims**
     (below);
   - `pgphoto.stage2ondisk` — the user's own `pgphoto`, reliability-patched
     (the same 14 bytes below) **and** on-disk-trampolined (byte-count-identical
     to stock; only ~719 bytes inside `.text` differ).
3. Replaces the **stock `ptp2.so` and `usb1.so`** (`/app/lib/libgphoto2/2.5.27.1/
   ptp2.so` and `/app/lib/libgphoto2_port/0.12.0/usb1.so`) with the same fresh
   2.5.34 builds. The swapped core dlopens its camlib/iolib from **these stock
   on-disk paths** at runtime — not from the `CAMLIBS` the wrapper exports — so the
   fresh driver must live there too, not only under `stage2/`.

The **loader's three shims** (each reads/writes only public libgphoto2 fields or
calls only public `gp_camera_set_config` APIs — no firmware code):

- **Storage shim** (`STAGE2_STORAGE_SHIM=1`, on): writes the Benro `_Camera`
  storage-type field so the app shows a memory card and raises **no "no card"
  warning**.
- **Config shims** (`STAGE2_TETHER_CAPTURE=1`, on): a `gp_camera_set_config`
  tree-walk plus a `gp_camera_set_single_config` hook that force Canon's
  `capturetarget` to **"Internal RAM"** (tethered capture). The Polaris drives
  configs via `set_single_config`, so that hook is the one that fires.

**Why tethered capture:** through the fresh 2.5.34 core, Canon's card-mode
post-capture `ObjectAddedEx` event is not delivered, so a card-target shot never
signals "file ready" and the download hangs. Internal-RAM capture uses the
`ObjectTransfer` path, which **does** fire — so the shot completes and both the
**JPEG and the RAW** download to the Polaris. See
[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

Every other `appfs` file — all other iolibs (`disk`/`serial`/`ptpip`/…), the
kernel, `rootfs.ubifs`, gimbal blobs, U-Boot env — is **byte-for-byte unchanged**.

### ptp2-only mode (`--ptp2-only`)

The conservative fallback changes up to **three** files in place:

1. **`/app/lib/libgphoto2/2.5.27.1/ptp2.so`** → freshly cross-compiled ptp2 camlib.
2. **`/app/lib/libgphoto2_port/0.12.0/usb1.so`** → freshly cross-compiled usb1
   iolib (skip with `--no-usb1` / `-NoUsb1`).
3. **`/app/bin/pgphoto`** → **14 bytes** of edits: 3 dispatch gates (load the new
   camlib instead of the compiled-in 2.5.27 copy), `resetUsb → return 0`, and
   skip the eager `ARG_LIST_FILES` full-card scan.

### Common to both modes

All patch sites are **discovered from `pgphoto`'s own symbol table** (the tool
refuses to run if it can't find/verify exactly what it expects), the rebuilt
libraries pass full ABI/glibc/symbol/`DT_NEEDED` verification, and `firmwareInfo`
is regenerated so the device's own MD5 check passes. The pgphoto reliability edits
(`resetUsb` + skip `ARG_LIST_FILES`) are present in **both** modes — in full mode
they live in the trampolined base binary.

> **Why the camlib historically needed 3 `pgphoto` gates:** Benro compiled the
> `ptp2` camlib *statically* into `pgphoto` and short-circuits its dispatch, so the
> on-disk `ptp2.so` is a dead filename marker in stock. ptp2-only mode re-routes
> those gates; **full mode** sidesteps the whole static core by trampolining every
> boundary call into the fresh 2.5.34 core. See
> [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for the full technical story
and [docs/TESTED.md](docs/TESTED.md) for exactly what was verified.

---

## Requirements

- **Docker** (Docker Desktop on macOS/Windows, or `docker` on Linux). That's it.
  All compilation and repacking happen inside the container; nothing else is
  installed on your machine.
- Your **stock `FwPkt`** (folder or `.zip`).

## Obtaining the stock firmware

This repository intentionally contains **no firmware**. Download the official
Benro Polaris firmware package yourself from Benro's downloads page:

**<https://www.benro.com/en/downloads/product/benro-polaris.html>**

You need the `FwPkt` package (a folder/zip containing `firmwareInfo` and
`camera/appfs.ubifs`). Verify you have **`FwVer 4.0.0.32`** (see the `FwVer`
file inside the package) — other versions are untested and the tool may refuse
them.

> Firmware is Benro's; this project neither hosts nor redistributes it.

---

## Usage

**macOS / Linux:**
```bash
./patch-polaris.sh --fwpkt /path/to/FwPkt --selftest
```

**Windows (PowerShell):**
```powershell
.\patch-polaris.ps1 -FwPkt C:\path\to\FwPkt -SelfTest
```

Options (both launchers):

| Option | Default | Meaning |
|---|---|---|
| `--fwpkt` / `-FwPkt` | *(required)* | Stock `FwPkt` folder (with `firmwareInfo`) or `FwPkt.zip` |
| `--libgphoto2` / `-Libgphoto2` | `2.5.34` | libgphoto2 release tag to build |
| `--out` / `-Out` | `./out` | Output directory |
| `--ptp2-only` / `-Ptp2Only` | off | Conservative fallback: keep the stock 2.5.27 core, swap only ptp2 + usb1 (+14-byte patch). Default is the **full** stack swap. |
| `--selftest` / `-SelfTest` | off | Emulate the driver load under qemu and confirm the R5 II registers |
| `--no-fix-typo` / `-NoFixTypo` | off | Keep libgphoto2's upstream `EOS 5Rm2` model-name typo |
| `--no-usb1` / `-NoUsb1` | off | (ptp2-only) Do **not** swap the `usb1` iolib; patch only the `ptp2` camlib + `pgphoto` |

Output:
- `out/FwPkt/` — the unpacked custom firmware
- `out/FwPkt.zip` — copy this to your SD card
- `out/stage2-ondisk/` — *(full mode)* reversible on-device test bundle
  (`ondisk/install_stage2.sh` installs it, `ondisk/restore_stock.sh` reverts)
- `out/licenses/` — *(full mode)* libgphoto2 `COPYING` (LGPL-2.1) + source offer

The first run builds the Docker image (a few minutes). Later runs are fast.

## Installing on the Polaris

Use **the same SD-card firmware-update procedure you already use for official
Benro updates**, but with the `FwPkt.zip` this tool produced. The device
verifies the package (MD5), reboots, and U-Boot writes it.

**To roll back:** run the same procedure with your **original stock `FwPkt`**.

---

## Verifying the build yourself

- `--selftest` loads the rebuilt `ptp2.so` against the device's *own* stock
  libgphoto2 core under emulation and prints the R5 II's registered USB id and
  capabilities. This proves ABI compatibility and driver registration; it
  **cannot** test real USB capture (there's no camera in the container).
- The tool aborts unless: the rebuilt driver's glibc symbol ceiling ≤ 2.24,
  every core symbol it imports is provided by the device's stock core, and the
  `pgphoto` patch changes **exactly 14 bytes**.
- For the **usb1** iolib it additionally aborts unless: soft-float EABI, glibc
  ceiling ≤ 2.24, it exports the three iolib entry points the port loader looks
  up, its `DT_NEEDED` is a **subset** of the stock `usb1.so`'s (no new shared
  library), every core/port symbol it imports is in the device's port core, and
  every `libusb_*` symbol it imports is in the device's own `libusb-1.0.so.0`.

## License

This project's own code (launchers, container scripts, `stage2_patch.py`, the
`stage2_loader.c` loader, the wrapper/install scripts) is **MIT** — see
[LICENSE](LICENSE). The libgphoto2 binaries the tool builds and ships in the
custom firmware are **LGPL-2.1**, rebuilt from the official upstream release with
two small documented source edits (a one-line ABI size pad and an EOS-init
error-tolerance patch; see [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) and
[NOTICE](NOTICE)). The tool ships **no firmware and no
proprietary/decompiled content**; it patches your own extracted `pgphoto`. Full
mode writes `out/licenses/` (LGPL `COPYING` + source offer) with the firmware. See
[NOTICE](NOTICE) for the full MIT-vs-LGPL breakdown.
