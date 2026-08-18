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
| License-friendly, like Aperion? | **Safe, but not by the route we first assumed.** Replacing `gsl-an` is *not* enough: the FITS layer the solver needs (`qfits-an`, incl. `anqfits.c`) is **GPL v2+** as well. The answer is a **process boundary** — the solver is its own GPL binary and nothing GPL is linked into `pgphoto`, `polestar_app`, or our MIT loader. Full analysis and the rules we build to: **[LICENSE-AUDIT.md](LICENSE-AUDIT.md)**. |
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

## Licensing — see [LICENSE-AUDIT.md](LICENSE-AUDIT.md)

Audited in full against the astrometry.net 0.98 tree. Short version:

- astrometry.net's own solver/util/libkd code is **BSD-3**, but the suite bundles
  four copyleft pieces. `gsl-an` (GPLv3) is removable with Aperion's BSD-3
  `gsl-shim`; **`qfits-an` (GPL v2+) is not removable** without writing a ~57
  entry-point FITS shim over cfitsio, and the solver needs it to read indexes.
- Therefore: **the solver runs as its own process**, distributed GPL v2-or-later.
  Our patcher stays MIT, Benro's binaries are untouched, and the phase-4 in-app
  hook *marshals* to the solver process rather than linking it.
- Avoid `simplexy`/`ctmf.c` (GPLv3) by feeding a star list; don't link
  `libcatalogs` (GPLv2+ Stellarium data); ship no index files and no test images.
- **Aperion has a live exposure here**: it still links `libqfits.a` and
  `libcatalogs.a` into an App Store binary. See the advisory in the audit.

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

### Protocol facts learned from the hardware

Two things the simulator got wrong until a real Polaris corrected them:

1. **The wire azimuth is signed.** `519`/`530` carry `yaw` as a value in
   **(−180, 180) measured westward**, not 0–360:
   `wire = (az > 180) ? 360 − az : −az`. A `519` carrying `yaw:256` is answered
   `ret:-1` and **the motors never move**. This is the same encoding the phone
   app uses.
2. **An unaligned mount refuses to point.** In Astro mode the mount reports
   `284@…track:3`, and while it does, every `519` comes back `ret:-1`. `527`
   (set compass) is *accepted* (`ret:0`) but does **not** clear it. What clears
   it is the **3-step `530` sequence**, after which `track` becomes `0` and
   gotos work. So alignment is a gate on pointing, not a refinement of it —
   which reorders the whole flow: **solve → 530 → goto**, never goto first.

The final `519` reply distinguishes the cases: `ret:0` = slew completed,
`ret:-1` = refused or aborted.

3. **Tracking needs the ASTRO MODULE physically attached.** ~~cause unknown~~
   Resolved by watching the phone app: `531` is refused with `ret:0` until the
   Benro astro module is connected, after which the *same* command we were
   already sending returns `{"tracking":true}` and the mount holds sky. Every
   protocol theory was wrong because the precondition was hardware. Verified:
   over 90 s the mount held RA/Dec to **6.8 arcsec**, against ~22 arcmin of
   drift for an untracked mount.

   **The app's real alignment recipe**, captured from the wire — it differs
   from the alpaca driver's in three ways worth knowing:

   ```
   285  mode:1                        drop out of Astro
   531  state:0                       tracking off
   285  mode:8                        back into Astro
   527  compass:0; lat; lng           compass RESET to zero (not az-180)
   519  yaw..pitch.. track:0          SLEW TO THE STAR FIRST
   530  step:1  yaw..pitch.. num:1    real values in step 1 (driver sends zeros)
   530  step:2  (identical values)    and there is NO step:3
   519  ...track:1                    goto again with tracking armed
   531  state:1                       start tracking
   ```

   Our `star-align` now matches the app (real values in both steps, no step 3).

4. **Superseded note on tracking refusal.** `531` answers `ret:0`
   (declined) under every condition tried. Ruled out on hardware:

   | hypothesis | result |
   |---|---|
   | needs a preceding goto with `track:1` | `519@ret:0;track:1`, still no motion |
   | needs a persistent client connection | held open 40 s, pose constant to 6 dp |
   | needs the app's full init handshake | all 18 frames replayed, still `ret:0` |
   | wrong mode | `mode:8` throughout |

   `polestar_app` is unstripped, so the task that would do the tracking —
   `SP_CreateTrackAuTask` — can be read directly: its only early gate is a
   re-entrancy flag (`cmp r3,#1` → log and return −1), which is *not* an
   alignment check. And our `531` produces **no error log at all**, so the
   request is declined in the message handler before it ever reaches that task.

   Leading theory: the mount will not track on a **self-referential
   alignment**. The `530` we sent told it "you are pointing where you already
   believe you are" — enough to clear `track:3`, but carrying no real sky
   information. The decisive test is capturing what the app sends when tracking
   genuinely starts, which needs a real star alignment and therefore darkness.

### Detecting the app's alignment: watch STATE, not the log

`polaris-autoalign` currently detects the app's alignment by tailing
`/app/Mlog.txt`, which records every frame `polestar_app` receives. That works
for the slew (`519 … track:0`) but is **not reliable for the confirm**:

- `polestar_app` **truncates that log continuously** — measured going from
  13439 bytes to 979 in twenty seconds.
- `tail -f` follows by *descriptor*, so the first truncation leaves it reading a
  stale offset and it silently never reports another line. Following by size and
  resetting on shrink (implemented) fixes that much.
- But a single critical line can still be **destroyed by truncation before it is
  read**, whatever the poll interval. Rehearsals showed exactly this: the solve
  fired reliably, the confirm was missed intermittently.

So log-watching is fine for the *coarse* trigger (we can afford to miss a slew;
another one is coming) and wrong for the *decisive* one.

A state-driven detector was built and then **disproved on hardware**: `284`
reports `track:3` only on a mount that has *never* been aligned. Driving the
app's own `285 mode:1 -> mode:8` sequence left it at `track:0` throughout, so
re-aligning an already-aligned mount produces no state transition at all. That
is the common case, so `track` cannot be the trigger.

**What is left, and what to build next.** Three signals exist and each is
partial:

| signal | good for | fails because |
|---|---|---|
| `Mlog.txt` lines | the coarse slew trigger | truncated continuously; a decisive line can vanish before any poll |
| `284 track` | a first-ever alignment | never returns to 3 once aligned |
| `518` pose | motion, always available | tells us the mount moved, not *why* |

**But live view changes the shape of this problem.** Blaine reports that the
Polaris' live view shows stars easily, and a live-view frame costs ~0.15 s to
fetch and extract (measured on device) against ~9.3 s for a full capture. If a
live-view solve lands in a couple of seconds, the daemon can simply **solve
continuously** and always hold a fresh answer. Detection then only has to decide
*when to inject*, not when to start solving — which makes a crude trigger
perfectly adequate, because the answer is already waiting.

That is worth confirming before building anything more elaborate. If it holds,
the trigger below is unnecessary.

Failing that, the trigger should be **pose-based**: a slew followed by a settle is the
observable signature of "the app went to its star and is waiting", and it works
regardless of alignment history. The ordering problem then remains — our `530`
must land *after* the user's, or theirs overwrites ours — which argues for
injecting once when the solve is ready and **re-injecting** a few seconds later
while the pose is unchanged, so we win the race whichever way it falls.

Note this is a *detection* problem only. Everything downstream — solve, gate,
conversion, `530` injection — is verified end to end on the device.

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

Ordered as Blaine specified: prove the licence, then prove the algorithm, then
let it touch motors, then let it touch the camera, and only then wire it into the
patcher.

**Phase 1 — licence safety. ✅ done.** [LICENSE-AUDIT.md](LICENSE-AUDIT.md)
establishes the component inventory and the process-boundary rule everything else
is built to.

**Phase 2 — bench the solver against ground truth, then on device.**
*Status: the host/qemu legs are **done** — see [BENCH-RESULTS.md](BENCH-RESULTS.md).
`polaris-solve` and `polaris-extract` are built for the device and validated;
what remains is running them on the Polaris itself.*
Two tiers, because they prove different things:

- *Tier A — solver correctness + speed.* astrometry.net's own `demo/` set is
  ready-made ground truth: `apod1–5.jpg` with matching `apod*.xyls` star lists,
  `demo/CREDITS` recording each field's size (90×60′, 4.5×3.4°, 8.4×6.3°,
  **34×24°**, 72×54°) and which index solves it, plus a bundled `index-4119.fits`
  (144 KB). The 34×24° and 72×54° frames are the closest analogue to a wide lens
  on a Polaris. Feeding the `.xyls` **skips star extraction entirely**, which is
  both the licence-clean path (rule 3) and the way to time *only* the matching
  algorithm. (Images are copyright APOD contributors — local use only, never
  committed.)
- *Tier B — the alignment maths.* Frames with **EXIF GPS + UTC** and a known
  pointing, to validate solved RA/Dec → alt/az → mount heading. This needs real
  captures from the rig; the demo images carry no GPS/time.

Harness: one `polaris-solve-bench` that takes a star list (or image) + a hint and
prints solved RA/Dec/roll/scale, wall-clock, and peak RSS. Run it three ways —
x86 host (sanity), **`qemu-arm-static` in the patcher container** (already in the
image), and **on the device over SSH** (which `--ssh-key` now makes trivial).
*Acceptance:* every demo field solves to its known answer, with a per-solve time
and RSS recorded for each of the three environments.

**Phase 3 — motors, safely.** Only after Phase 2 has real numbers. Drive the
mount over the loopback control protocol with hard safety rails: slew limits, an
abort path, no motion without a solve that passes the RMS gate, and a dry-run
mode that prints the moves it *would* make. Then the multi-frame sweep +
q-method pointing model. *Acceptance:* a cold, compass-free start lands known
targets, and every failure mode stops the motors rather than guessing.

**Phase 4 — the real camera.** Requires the capture-path fix below (RAW+JPEG on
the camera's card **and** a copy on the Polaris SD). *Acceptance:* end-to-end on
the R5 II — capture, solve, sync, track — with the files where they belong.

**Phase 5 — patcher integration.** Only once Phases 2–4 say the resource and
timing budget is real. Additive appfs files + one boot hook, revertible by
reflashing stock, same contract as the rest of this tool.

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
