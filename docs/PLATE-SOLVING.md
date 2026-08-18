# On-device plate solving for the Benro Polaris — feasibility + design

**Status: design / feasibility study. No code yet.** Every "device fact" below was
read out of the stock `FwVer 4.0.0.32` images (`rootfs.ubifs`, `appfs.ubifs`,
`uImage`, U-Boot `config`) or out of the two research repos named at the end;
every number that is an *estimate* is labelled as one.

## The goal

Replace compass + single-star alignment with **automatic astrometric alignment**:
the Polaris takes a few frames around the sky, plate-solves each one, fits a
pointing model from the solutions, and syncs itself — no compass, no manual star
centring, and far better tracking than stock. Two front ends:

1. **Inside the Benro app** — hooked into the astro-calibration flow the app
   already has, so the user just runs "align" and it is silently better.
2. **Standalone HTTP** — a small on-device web page + REST API (an
   ASCOM-Alpaca-shaped one) so the Polaris can be driven without the app.

---

## Verdict up front

| Question | Answer |
|---|---|
| Enough CPU/RAM on the chipset? | **Yes, with margin.** Dual-core ARM (Cortex-A7-class, v7-A + **NEON/VFPv4**), **1536 MB RAM** (`mem=1536M` in the U-Boot bootargs), Linux 4.9.37. A wide-field solve is a seconds-to-tens-of-seconds job here, not minutes — *estimate, must be measured (Phase 0)*. |
| Enough storage? | **Yes, if the index files live on the microSD.** The appfs partition has **~20.4 MB** of headroom (81 MB partition, 64.5 MB stock image) — enough for the solver + a tiny index, nowhere near enough for a real index set (Aperion ships 182 MB). `/app/sd` is the user's microSD and is already used by the app (`/app/sd/starskyStack/`). |
| License-friendly, like Aperion? | **Yes — the same trick works, and it is the *only* hard licensing problem.** astrometry.net's own code is BSD-3; its bundled `gsl-an` is **GPL-v3** and is what forces the whole work to GPL. Aperion already replaced it with a BSD-3 `gsl-shim`. Porting that shim off Accelerate is ~7 numeric routines. |
| Can it be wired into the app's own astro calibration? | **Yes, and more cheaply than expected.** `/app/bin/polestar_app` is **not stripped, ships full DWARF, and exports 17,440 functions** — including `SP_OneStarCal`, `SP_CreateStarskyStackTask`, `SP_StarskyStackMsgFromAppProc` and a whole `starskystacker` C++ class with OpenCV statically linked in. |
| Multi-frame solve? | **Yes** — and it is the right design. Per-frame WCS → rigid-rotation fit (Davenport q-method) → RMS gate, the same shape as Aperion's `PointingModel`, but fed by solves instead of the mount's own pose telemetry (which is the thing that was unreliable). |
| Can the patcher install all this? | **Yes.** Same mechanism as today: extra files in the appfs + one boot hook. The `--ssh-key` work already proved the hook path (`/app/bootapp` runs `/app/network_telnetd.sh` if present). |

**The one thing that must change first:** the current R5 II capture shim forces
`capturetarget = Internal RAM`, so **nothing is written to the camera's card**.
That is unacceptable for real shooting and is also wrong for this feature. See
[Capture path](#capture-path-must-write-to-the-camera-card-too).

---

## What the device actually is (verified)

| | |
|---|---|
| SoC | HiSilicon **Hi3559V200** (`uImage` device tree: `hisilicon,hi3559v200`, `arm,cortex-a7`, `arm,cortex-a7-gic/-pmu`) |
| Cores | **2** (device tree has `cpu@0` and `cpu@1`) |
| RAM | **1536 MB** (`mem=1536M` in `bootargs`) |
| FP | **VFPv4 + NEON present and used.** `polestar_app`'s ARM build attributes: `Tag_FP_arch=5` (VFPv4), `Tag_Advanced_SIMD_arch=2` (NEON), **no** `Tag_ABI_VFP_args` → the soft-float *calling convention* with hardware FP instructions (`-mfloat-abi=softfp`). Benro's own build even tags `Cortex-A53`/v8-A. |
| Kernel / libc | Linux 4.9.37, glibc 2.24, `/` mounted **rw** (`root=ubi0:ubifs … rw`) |
| Flash | `mtdparts`: uImage 5M, **rootfs 40M** (21.1 MB used), **appfs 81M** (64.5 MB used → **~20.4 MB free**) |
| Removable | microSD at `/app/sd` (firmware updates + `/app/sd/starskyStack/` already used by the app) |

> **Build target for anything we add:** `-march=armv7-a -mfpu=neon-vfpv4
> -mfloat-abi=softfp`, glibc ceiling ≤ 2.24 — i.e. the existing debian:9 cross
> toolchain in `docker/Dockerfile`, plus the NEON flags. That is safe on both a
> Cortex-A7 and the A53 Benro's own tags claim, and keeps the soft-float calling
> convention every existing device binary uses.

## What is already on the device that we can reuse

| Asset | Where | Why it matters |
|---|---|---|
| **OpenCV** (statically linked) | inside `polestar_app` (`cv::calibrateCamera`, `cv::KeyPoint`, `cv::linearPolar`, …) | The app already does blob/feature work on sky images. Static, so *not* linkable from another process — but it proves the workload fits, and it is reachable from **inside** the app. |
| **`starskystacker`** class | `polestar_app`: `getStars(cv::Mat&,float,float,float)`, `findMatchingStarsMatrix()`, `tryFindMatrix()`, `raw2mat()`, `addImg()`, `getStackedImg()` | A **working, shipping star extractor + matcher on this exact hardware**, with an image-ingest path that reads files by path from `/app/sd/starskyStack/`. |
| **`SP_OneStarCal`**, `SP_GetCalibrate`, `MagCalibrateTaskV2`, `SP_CreateGotoAuTask`, `SP_GotoAuUartSend*`, `Sync` | `polestar_app` (unstripped + DWARF) | The exact "methods already called during astro calibration" the feature should stub into. |
| **XEphem `libastro`** | `polestar_app` (`sp_astro/aa_hadec.c`, `circum.c`, `deltat.c`, `aberration.c`, …) | Precession/refraction/alt-az conversion already on device and already trusted by the mount. |
| **lighttpd** | `/app/bin/lighttpd` (565 KB) + `/etc/lighttpd/lighttpd.conf` (docroot `/app`, `0.0.0.0:80`) | A real HTTP server is **already shipped**, pre-configured, and not started at boot. |
| **busybox `httpd`** | `/bin/busybox` applet list | Fallback: CGI-capable HTTP server in the rootfs, zero bytes added. |
| **gphoto2 CLI**, `gdbserver`, `frpc` | `/app/bin/` | Debugging + camera poking without writing new tooling. |
| **OpenSSH** | `/usr/local/bin/sshd`, started by `rcS` | Already usable for development (see `--ssh-key`). |
| **TCP control protocol** | `polestar_app`'s app port; messages are ASCII `NNN@payload#` (284 mode, 517 orientation, 518 position, 519 goto, 525, 527, 531 track; mode **8 = Astro**) | A local daemon can drive the mount over **loopback** with the same commands the phone app uses — **no `polestar_app` patching required for v1**. |

---

## Licensing — the model to copy from Aperion

**The problem.** astrometry.net's own `LICENSE` says it plainly: the team's code is
3-clause BSD, *but* because the tree bundles GPL libraries — "including a vendored
GSL" — "the whole work must be distributed under the GPL version 3 or later."

**Aperion's fix, which we reuse.** `Vendor/astrometry-net` is pinned at 0.98 and
the build **deletes `gsl-an` entirely** and links
`scripts/astrometry/gsl-shim/` instead — 713 lines, `SPDX-License-Identifier:
BSD-3-Clause`, providing every `gsl_*` symbol astrometry.net references. With
`gsl-an` gone the shipped work is BSD-3 (astrometry.net) + BSD-ish (cfitsio) +
BSD-3 (our shim).

**What porting it to the Polaris costs.** The shim's plumbing (block/vector/matrix/
permutation alloc, get/set, views) is portable C already. Only the heavy routines
delegate to Apple's Accelerate, and there are just **seven**:

```
gsl_linalg_LU_decomp      gsl_linalg_LU_invert
gsl_linalg_QR_decomp      gsl_linalg_QR_lssolve
gsl_linalg_SV_decomp      gsl_linalg_SV_decomp_jacobi
gsl_blas_dgemm
```

astrometry.net calls these on **tiny** matrices (3×3 / 4×4 / N×3 least squares in
the WCS fit), so the Polaris backend does **not** need BLAS/LAPACK at all — a
self-contained Jacobi SVD + Householder QR + LU with partial pivoting is a few
hundred lines of our own code, which also keeps the licence clean and the binary
small. (Aperion already has the Jacobi eigen-solver logic in
`MountKit/PointingModel.swift` to mirror.) A netlib LAPACK/CLAPACK backend
(BSD-3) stays available as a fallback if numerical parity ever gets fussy.

**Also check before shipping (open item):** `qfits-an` inside the astrometry tree
has no `LICENSE` file of its own; qfits' ESO origin is GPL-flavoured. If the
solver path we use pulls in `qfits-an`, that must either be replaced (we only
need to *read* a handful of FITS keywords, or skip FITS entirely by feeding the
solver an in-memory star list) or the licence posture changes. **Aperion should
be audited for the same question.**

**Index files.** The astrometry.net 4100/4200 index series are published for free
download but are *data*, not our code; ship none of them in the repo. The patcher
should fetch/verify them at the user's request, exactly like Aperion's
"offer to download a better index" flow, and stage them on the **microSD**.

**And the patcher's own posture stays the same as today:** this repo ships no
firmware and no binaries; everything is built from source in the container at run
time, with `out/licenses/` carrying the notices for what ends up in the image.

---

## Architecture

Three layers, deliberately separable so each can ship (and be reverted) alone.

```
   ┌─────────────────────────── Benro Polaris (on device) ────────────────────────────┐
   │                                                                                  │
   │  polestar_app  ──TCP loopback (NNN@…#)──┐                                        │
   │   │  (mount, goto, sync, astro mode 8)  │                                        │
   │   │                                     ▼                                        │
   │   │                            ┌──────────────────┐    HTTP :8080  ┌───────────┐ │
   │   │                            │   polarissolved  │◄──────────────►│  web UI + │ │
   │   │                            │  (our daemon)    │   Alpaca-ish   │  REST API │ │
   │   │                            └────────┬─────────┘                └───────────┘ │
   │   │                                     │ libpolarissolve (static)               │
   │   │                                     │  = astrometry.net solver + BSD gsl-shim│
   │   │                                     │                                        │
   │  pgphoto (our stage2 loader already inside it)                                    │
   │   └── capture ⇒ JPEG on /app/sd/…  ⇒ solver reads it                              │
   │                                                                                  │
   │  index files on microSD:  /app/sd/astrometry/index-41xx.fits                      │
   └──────────────────────────────────────────────────────────────────────────────────┘
```

### Layer 1 — `libpolarissolve` (the solver)

- astrometry.net 0.98 `solver/` + `libkd` + `util` + `catalogs`, cross-built with
  the existing debian:9 toolchain, **without** `gsl-an`, **with** the ported
  BSD-3 gsl-shim, static, `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp`.
- Input: a star list (x, y, flux) + a hint (approximate FOV from focal length,
  approximate alt/az from the mount, time from the RTC). Hints cut the search
  space enormously and are the difference between "seconds" and "give up".
- Output: WCS (RA/Dec of centre, roll, pixel scale) + match quality.
- **Star extraction:** start with astrometry.net's own `simplexy`; the fallback if
  it is too slow is the device's *proven* path — `starskystacker::getStars()`
  inside `polestar_app` — which is only reachable from Layer 3.

### Layer 2 — `polarissolved` (daemon + HTTP)

- Speaks the mount's own ASCII protocol on **loopback** (`NNN@payload#`): read
  517/518 pose, drive 519 goto, 531 track, 527/`Sync`.
- Runs the **multi-frame align sequence** (below).
- Serves a single-page control UI + JSON API. Two options, both zero-new-daemon:
  **(a)** the shipped `lighttpd` with our CGI, **(b)** busybox `httpd`. A tiny
  built-in HTTP server inside the daemon is the third option and probably the
  cleanest for long-poll progress.
- **Stretch goal that falls out for free:** shape the REST surface as **ASCOM
  Alpaca** (`/api/v1/telescope/0/…`) and NINA/Stellarium/PHD2 can talk to the
  Polaris **directly**, with no PC-side bridge — i.e. `alpaca-benro-polaris`
  running *on the mount*.
- Enabled/disabled by a flag file on the SD card so a user can turn it off
  without reflashing.

### Layer 3 — in-app hook (phase 2)

`polestar_app` is unstripped with DWARF, which makes it the *same* engineering
problem the `stage2` on-disk trampoline already solved for `pgphoto`:

- `SP_OneStarCal` is the natural interception point — when the user runs the
  app's astro calibration, our code runs the multi-frame solve instead and hands
  back a better calibration through the same call the app already makes.
- `SP_StarskyStackMsgFromAppProc` / `SP_CreateStarskyStackTask` show how the app
  ingests images by path; `starskystacker::getStars()` is a ready-made on-device
  star extractor we can call *in-process* rather than re-implementing.
- Same safety rules as `stage2`: discover every site from the symbol table, fail
  closed on any mismatch, keep the binary byte-count-identical, LD_PRELOAD-style
  revertibility.

**Phase 1 does not need any of this.** Layer 2 driving the mount over loopback is
enough for a working "solve → sync → track" button in a browser, and it is
reversible by deleting one file.

### The multi-frame solve sequence

1. Mount points at a first target (or wherever it is), camera takes a short,
   wide-field, high-ISO frame.
2. Frame lands on the Polaris SD (and, per the requirement below, on the camera's
   own card too). Solver runs → WCS #1.
3. Mount slews a known amount (e.g. 3–5 points spread ≥ 30° apart in az and alt,
   avoiding the zenith), repeat.
4. Fit a **rigid rotation** from the (commanded mount vector → solved sky vector)
   pairs — Davenport q-method + Jacobi eigen-solve, the same maths as Aperion's
   `PointingModel` — yielding azimuth offset **and** base tilt.
5. **Gate it:** require ≥ 3 points and RMS ≤ a threshold; discard and report
   otherwise. (Aperion's field lesson: a bad model is far worse than none — an
   ungated fit flung gotos ~270° off.)
6. Sync the mount from the *solved* heading, then track.

Critically, this uses the **stars** for heading, not the Polaris' 518 pose
telemetry, which Aperion's rig testing found **freezes mid-operation** (logged
"518 frozen for 12s") and produced a garbage 58° RMS calibration. Doing the fit
on-device removes the phone round-trip but does **not** remove that hazard — the
model must be built from solves, and any use of 518 must be treated as untrusted.

---

## Capture path: must write to the camera card too

**Today's behaviour is wrong for this and for normal shooting.** The stage2
loader's `STAGE2_TETHER_CAPTURE` shim forces Canon `capturetarget = Internal RAM`
because, through the fresh 2.5.34 core, a card-target shot's post-capture
`ObjectAddedEx` event never arrives in the Polaris' event loop, so the download
hangs. The consequence is that **the R5 II keeps nothing on its own card** — an
unacceptable trade for a real shoot, and bad for astro work where the RAW on the
card is the deliverable.

**Requirement:** capture must write to the **camera's card** *and* download a copy
to the Polaris microSD.

Candidate fixes, cheapest first (all inside the loader we already own):

1. **Card target + polled fetch.** Set `capturetarget = Memory card`, then, instead
   of waiting for the event, wrap `gp_camera_wait_for_event` so that on timeout it
   asks the camera for the newest object on the card storage and downloads that.
   Keeps the RAW+JPEG on the card; the Polaris gets its copy.
2. **Card target + fixed event handling.** Find out *why* `ObjectAddedEx` is lost
   (event queue drained elsewhere? wrong event mode after `SetRemoteMode`?) and
   fix it in the ptp2 driver we already build from source. Cleanest if it works.
3. **Dual-write** — keep Internal-RAM capture for the solver's fast frames (small
   JPEG, never touches the card, no shutter-count/card-wear cost) **and** use the
   card path for the user's real exposures. This is likely the end state: the
   plate-solver wants a small throwaway JPEG, the astrophotographer wants the RAW
   on the card.

This is tracked as **Phase 0.5** below — it is a prerequisite for the feature and
a bug fix in its own right.

---

## Resource budget

| Resource | Budget | Notes |
|---|---|---|
| appfs (flash) | **~20.4 MB free** | solver + shim + daemon + web UI ≈ **2–5 MB** *(estimate)*. Comfortable. |
| microSD | user's card | index files: 0.7 MB (`4119`) … 182 MB (full 4108-4119 set as Aperion ships). Only the scales matching the user's lens are needed. |
| RAM | 1536 MB total | astrometry.net memory-maps index files; a solve with wide-field indices is tens of MB *(estimate)*. Non-issue at this RAM. |
| CPU | 2 × ~1 GHz ARM, NEON | **The unknown.** Must be measured (Phase 0). |
| Thermals/power | — | The Polaris is battery-powered and unventilated; a solve pinning both cores for 60 s has a real cost. Budget one solve per pointing, not continuous solving. |

---

## Phased plan

**Phase 0 — measure, before writing anything real.**
Cross-build astrometry.net (GSL-free) for ARM in the existing container and run a
real solve **under `qemu-arm-static`, which is already in the image**, against a
sample wide-field frame + a small index. That yields an order-of-magnitude
per-solve time and peak RSS, and it costs one afternoon. If the emulated number
is catastrophic, we learn it before building any product.
*Acceptance:* a solved WCS from a known frame, with wall-clock + RSS recorded.

**Phase 0.5 — fix the capture path** (see above). *Acceptance:* on the user's R5
II, a capture leaves the RAW+JPEG on the camera card **and** a copy on the Polaris
SD, with no hang.

**Phase 1 — `libpolarissolve` + the BSD gsl-shim port.**
*Acceptance:* the same frame solves natively on the device, timed; licence audit
written (including the `qfits-an` question).

**Phase 2 — `polarissolved` + web UI.** Loopback mount control, one-shot
"solve & sync" from a browser page. *Acceptance:* a solve moves the mount to the
right place from a cold, compass-free start.

**Phase 3 — multi-frame pointing model.** N-point sweep, q-method fit, RMS gate.
*Acceptance:* pointing accuracy across the whole sky beats a compass align,
measured against known targets.

**Phase 4 — in-app hook at `SP_OneStarCal`.** *Acceptance:* the app's own astro
calibration silently uses the solver, and the patcher can revert it.

**Phase 5 — Alpaca surface.** NINA/Stellarium talk to the mount with no PC bridge.

Each phase must be installable by the patcher as **additive appfs files + one boot
hook**, and revertible by reflashing stock — the same contract the rest of this
tool keeps.

---

## Risks / what would kill this

- **Solve time on a Cortex-A7.** The single biggest unknown. Mitigations: strong
  hints (mount pose + focal length + time), small index scales only, downsampled
  frames, `starskystacker`'s existing extractor.
- **Only one client on the mount's control port.** If `polestar_app` accepts a
  single TCP client, our daemon may fight the phone app. **Unverified — must be
  tested early.** Falls back to Layer 3 (in-process hook), which has no such
  conflict.
- **`qfits-an` licence** (above) — could force a FITS-free feed path.
- **Camera/USB ownership.** `pgphoto` owns the camera; the solver must obtain
  frames *through* it (or through files it writes), never by opening the USB
  device itself.
- **Thermal/battery** on long sequences.
- **Sky conditions** — clouds/moon/glare produce false or failed solves; the RMS
  gate and "discard, don't apply" rule are mandatory, not optional.

## Sources

- Device facts: stock `FwPkt` `FwVer 4.0.0.32` images, read directly.
- `polestar_app` symbol/DWARF survey: this study.
- **`polaris-pgphoto-rebuild`** (private): DWARF-accurate reconstruction of
  `pgphoto`, `IPC_PROTOCOL.md` (SysV message-queue command channel,
  `cb_arg_run` dispatch), `CAMERAINIT_ANALYSIS.md`. Its README already names
  "running an on-device plate solver / auto-calibration directly on the Polaris"
  as its purpose. **Not distributable — reference only.**
- **Aperion**: `Vendor/astrometry-net` @ 0.98, `scripts/astrometry/build-deps.sh`,
  `scripts/astrometry/gsl-shim/` (BSD-3 GSL replacement),
  `docs/astrometry/ROADMAP.md`, `MountKit/PointingModel.swift` (q-method),
  and the rig-test findings about 518 pose telemetry.
- `alpaca-benro-polaris` (research copy): the `NNN@payload#` control protocol and
  opcode meanings.
