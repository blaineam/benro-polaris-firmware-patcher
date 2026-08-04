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

Result on a real R5 Mark II: **immediate detection, live view, controls, and
capture.** See [docs/TESTED.md](docs/TESTED.md).

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

Exactly two files inside the firmware's `appfs`, nothing else:

1. **`/app/lib/libgphoto2/2.5.27.1/ptp2.so`** → freshly cross-compiled from the
   libgphoto2 version you choose (default 2.5.34), built for the device's exact
   ABI (32-bit ARM, soft-float EABI, glibc 2.24).
2. **`/app/bin/pgphoto`** → **14 bytes** of edits, all reversible:
   - **3 gates** (`mov r3,r0` → `mov r3,#0`) that make it load the driver above
     instead of its compiled-in 2.5.27 copy;
   - **`resetUsb` → return 0** (stops the cold-connect USB-reset re-enumeration
     storm);
   - **skip `ARG_LIST_FILES`** in `cameraInit` (stops the multi-minute full-card
     PTP file scan that blocks live view/capture at connect).

All patch sites are discovered from `pgphoto`'s symbol table; the tool refuses
to run if it can't find exactly what it expects.

Every other file (kernel `uImage`, `rootfs.ubifs`, gimbal MCU blobs, U-Boot
environment `config`, all other `appfs` files) is copied **byte-for-byte
unchanged**. `firmwareInfo` is regenerated so the device's own MD5 check passes.

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
| `--selftest` / `-SelfTest` | off | Emulate the driver load under qemu and confirm the R5 II registers |
| `--no-fix-typo` / `-NoFixTypo` | off | Keep libgphoto2's upstream `EOS 5Rm2` model-name typo |

Output:
- `out/FwPkt/` — the unpacked custom firmware
- `out/FwPkt.zip` — copy this to your SD card

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
  `pgphoto` patch changes **exactly 3 bytes**.

## License

MIT — see [LICENSE](LICENSE). libgphoto2 is built from source under its own
license (LGPL-2.1).
