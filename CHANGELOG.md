# Changelog

## Unreleased — full-libgphoto2 stack swap is now the DEFAULT (hardware-verified)

The patcher now replaces the **entire** libgphoto2 stack by default — core + port
+ ptp2 camlib + usb1 iolib, all fresh 2.5.34 — instead of only the ptp2 camlib +
usb1 iolib. The old camlib/iolib-only swap becomes the opt-in **fallback**.

### Changed
- **Default mode is now `full`.** No mode flag = full-libgphoto2 swap. The
  conservative legacy swap (keep the stock 2.5.27 core; swap only ptp2 + usb1 +
  the 14-byte `pgphoto` patch) is now **`--ptp2-only` / `-Ptp2Only`**.
- Docs lead with full mode as the default and the hardware-verified path;
  ptp2-only is documented as the fallback.

### Added — full mode
- **On-disk trampoline core swap.** `pgphoto` (non-PIE `ET_EXEC`) has the 64
  libgphoto2 boundary functions it calls (`gp_*`/`gp_port_*`) rewritten in the
  file to an absolute indirect jump through a pointer slot; a fresh-2.5.34 core is
  `dlopen`ed and each slot filled at startup. **No runtime `.text` mprotect, no
  `/proc/self/mem`** (three earlier runtime-patching loaders were crashed/refused
  by the Hi3559V200 kernel). The trampolined binary is byte-count-identical to
  stock (only 719 `.text` bytes differ; entry point unchanged).
- **Loader `libpolaris_stage2.so`** (`container/stage2_loader.c`): mmaps a fresh
  `MAP_FIXED` slot page at `0x30000000` (with a `/proc/self/maps` overlap
  pre-check), fail-closed `abort_stub` baseline for all slots, `dlopen`s core+port
  by absolute path (env-less), `SIGSEGV` pinpoint handler + checkpoints.
- **`container/stage2_patch.py`** — generic on-disk trampoline patcher (fail-closed
  on any undersized/unresolved/out-of-segment boundary entry; derives the
  reliability-patch sites from the stock↔base diff and refuses if a trampoline
  would clobber one).
- **`container/build_fullstack.sh`** (+ `FULLSTACK=1` and `POLARIS_DBG=1` in
  `build_ptp2.sh`) builds `libgphoto2.so.6` + `libgphoto2_port.so.12` alongside
  ptp2/usb1, with `struct _Camera` padded to 4140 bytes (Benro-tail ABI parity; an
  interop size constant, ABI-inert, nothing proprietary), and skips the ptp2
  trampoline shim (the fresh core exports `gp_filesystem_set_info_dirty` natively).
- **`container/dbg_patch.py`** (POLARIS_DBG, shippable, **no tracing**) makes the
  Canon EOS init-time drains non-fatal (`config.c` check_eos_events ×13; `library.c`
  keep_device_on ×3 + check_eos_events ×1) so `camera_init` completes as the real
  Canon driver instead of falling back to the generic PTP class driver. A documented
  LGPL source modification (see `NOTICE`). `trace_patch.py` remains a dev-only
  `TRACE=1` diagnostic that is never shipped.
- **Loader shims** in `stage2_loader.c` (both on via the wrapper): `STAGE2_STORAGE_SHIM`
  writes the Benro `_Camera` storage-type so the app shows a card (**no "no card"
  warning**); `STAGE2_TETHER_CAPTURE` forces Canon `capturetarget` to "Internal RAM"
  via `gp_camera_set_config` + `gp_camera_set_single_config` (the Polaris drives
  configs via `set_single_config`, so that hook is the one that fires). Internal-RAM
  capture uses the `ObjectTransfer` path (card-mode `ObjectAddedEx` is not delivered
  through the fresh core), so a shot completes and **both JPEG and RAW** download.
- **Self-driving wrapper** (9-line, no logging) installed as `/app/bin/pgphoto`
  (exports CAMLIBS/IOLIBS/LD_LIBRARY_PATH/LD_PRELOAD + the two shim toggles, execs
  the trampolined binary from `/app/lib/stage2`).
- **Stock-path camlib/iolib placement.** The fresh `ptp2.so`/`usb1.so` are written
  to the stock on-disk paths (`/app/lib/libgphoto2/2.5.27.1/ptp2.so`,
  `/app/lib/libgphoto2_port/0.12.0/usb1.so`) **as well as** the `stage2/` tree — the
  swapped core loads its camlib/iolib from the stock paths at runtime, not from the
  exported `CAMLIBS`. Stock perms preserved (camlib `0750`). Every other appfs file
  (other iolibs, kernel, rootfs, gimbal, U-Boot env) stays byte-identical.
- **Reversible on-device bundle** `out/stage2-ondisk/` (`install_stage2.sh` /
  `restore_stock.sh`) to test before flashing, and `out/licenses/` (libgphoto2
  `COPYING` LGPL-2.1 + source offer). New top-level `NOTICE` (MIT-vs-LGPL layout).

### Verified — full mode
- **On real hardware (Canon EOS R5 Mark II + Benro Polaris):** flashed and
  confirmed — settings stick, live view, **no card warning**, and capture
  downloads **both JPEG and RAW**. Cold boot → `slots filled 64/64` →
  `gp_camera_init ret 0`.
- **Deterministic reproduction (in-container, default full mode):** the public
  patcher reproduces **every** hardware-validated component byte-for-byte —
  core `b4c7ec31`, port `aa3ff350`, ptp2 `9bdbd13d` (at both the `stage2` and stock
  `2.5.27.1` paths), usb1 `5199e973` (both paths), loader `74f681de`, trampolined
  binary `a83ac7bb`, wrapper `868c3097`. Only the whole-image `appfs.ubifs` md5
  shifts between runs (UBIFS per-inode mtime); every file inside is byte-identical.
  See [docs/TESTED.md](docs/TESTED.md).

## Previously — usb1 iolib swap (offline-built, pending on-device verification)

Extends the patcher from a camlib-only swap toward a full libgphoto2-stack
update: it now also rebuilds and swaps the **`usb1` port iolib** (the USB
transport), not just the `ptp2` camlib.

### Added
- **Swap `/app/lib/libgphoto2_port/0.12.0/usb1.so`** alongside `ptp2.so`, built
  from the same libgphoto2 release. Investigation confirmed the port layer is
  **`dlopen`-loaded** (`gp_port_set_info` → `lt_dlopenext` + `lt_dlsym(
  "gp_port_library_operations")`, no static short-circuit like the camlib), so
  replacing the on-disk iolib takes effect with **no `pgphoto` edit**.
- The rebuilt `usb1.so` is **libusb-based**, matching stock: linked against the
  device's **own** `libusb-1.0.so.0` soname, ABI-matched (soft-float EABI,
  glibc-2.24 ceiling). Its `DT_NEEDED` equals the stock iolib's exactly (the
  spurious libtool-over-linked `libltdl.so.7` is dropped). Corrects an earlier
  assumption that stock USB was raw-usbfs: `pgphoto` has no libusb symbols
  because libusb is a dependency of the *dlopen'ed* `usb1.so`, not of `pgphoto`.
- Fail-safe usb1 verification (aborts on mismatch): soft-float ABI, glibc ≤ 2.24,
  exports the three iolib entry points, `DT_NEEDED` ⊆ stock `usb1.so`, all
  core/port symbols resolvable against the device port core, and all `libusb_*`
  symbols resolvable against the device's own `libusb-1.0.so.0`.
- `--no-usb1` / `-NoUsb1` (env `SWAP_USB1=0`) to keep the legacy camlib-only
  behaviour. Verified: with `--no-usb1` the output differs from stock in exactly
  two files (`pgphoto`, `ptp2.so`), and `ptp2.so` is byte-identical whether or
  not usb1 is swapped (enabling usb1 does not perturb the proven camlib build).
- Docker image gains `libusb-1.0-0-dev` (cross headers) and `patchelf`.

### Verified (offline / in emulation)
- End-to-end pipeline is exit-code clean; the repacked appfs round-trips with
  **only** `pgphoto`, `ptp2.so`, and `usb1.so` changed — all other iolibs
  (`disk`/`serial`/`ptpip`/`usbscsi`/`usbdiskdirect`) byte-identical; UBIFS
  `space_fixup` preserved (repack path unchanged).

### Not yet verified
- The usb1 swap has **not** been tested on real hardware. Camlib + `pgphoto`
  behaviour is unchanged from the version below and remains device-verified.

## Previously — verified working on real hardware

First working version. **Confirmed on a physical Canon EOS R5 Mark II + Benro
Polaris:** immediate detection, live view, camera controls, and capture.

### Added
- Single-image Docker pipeline (debian:9 = glibc-2.24 cross toolchain +
  mtd-utils + ubi_reader) driven by cross-platform launchers
  `patch-polaris.sh` (macOS/Linux) and `patch-polaris.ps1` (Windows).
- Cross-builds the `ptp2` camlib from any libgphoto2 release (default 2.5.34),
  ABI-matched to the device (soft-float EABI, glibc-2.24 ceiling), linked
  against the device's own libs. Drops the `camera_keep_device_on` heartbeat
  and the `camera_exit` `SetRemoteMode` toggle for Polaris reliability.
- **`pgphoto` patch (14 bytes, all symbol-discovered, reversible):**
  - three static-dispatch gates (`mov r3,r0`→`mov r3,#0`) so the rebuilt driver
    loads instead of the compiled-in 2.5.27 copy;
  - **`resetUsb` → return 0** — stops the `USBDEVFS_RESET` re-enumeration storm
    that made cold connects grind (camera USB device number walking 11→12→13…);
  - **skip `ARG_LIST_FILES` in `cameraInit`** — stops the multi-minute full-card
    PTP file scan that held the camera "busy" and blocked live view / shutter.
  - the `gp_filesystem_set_info_dirty` trampoline target for the rebuilt driver.
- Faithful appfs extract/repack (`ubireader -k` + `mkfs.ubifs`/`ubinize`) using
  geometry read from the stock image; preserves the UBIFS **space_fixup** flag
  (prevents reboot-hang); regenerates `firmwareInfo`.
- `--selftest`: qemu-emulated driver load proving the R5 II registers with
  capture caps against the device's own stock core.
- Safety gates: glibc ceiling ≤ 2.24, all core symbols resolvable, exactly
  14 patched bytes (3 gates + `resetUsb` + list-files), the `resetUsb` prologue
  and exactly one `ARG_LIST_FILES` dispatch present;
  kernel/rootfs/gimbal/U-Boot-env left byte-identical.
- Corrects the upstream libgphoto2 2.5.34 `EOS 5Rm2` → `EOS R5m2` model typo.

### Verified
- On real hardware: Benro Polaris **FwVer 4.0.0.32** + libgphoto2 **2.5.34** +
  Canon EOS R5 Mark II. See [docs/TESTED.md](docs/TESTED.md).

### Not tested
- Any camera other than the R5 Mark II; any firmware other than 4.0.0.32.
