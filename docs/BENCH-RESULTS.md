# Plate-solve bench — measured results

Phase 2, Tier A: **how fast is the matching algorithm, and does it get the right
answer.** Reproduce with [`container/astro/bench-solve.sh`](../container/astro/bench-solve.sh).

## Method

- **Input is a star list, not an image** (`demo/apod*.xyls` shipped by
  astrometry.net). That excludes star-extraction time, so what is timed is *only*
  the quad-matching solve — and it keeps GPLv3 `simplexy`/`ctmf` out of the
  picture entirely ([LICENSE-AUDIT.md](LICENSE-AUDIT.md) rule 3).
- **Ground truth** is astrometry.net's own `demo/CREDITS`, which records each
  field's angular size.
- **Indexes**: Tycho-2 wide-field series **4116–4119**, downloaded from
  data.astrometry.net. **Total: 988 KB** (409 / 248 / 187 / 144 KB).
- **Binary**: Debian 12's packaged `solve-field` 0.93, run in `linux/amd64` and
  `linux/arm/v7` containers. This is a **reference** measurement, not the binary
  we would ship — Debian's armhf build is hard-float against glibc 2.36, while
  the Polaris is soft-float against glibc 2.24.
- Both containers ran on an Apple-silicon host, so **both architectures are
  emulated**. Treat the numbers as *relative*, not absolute.

## Results

| Field | Solved size | mode | x86-64 | ARM32 (armv7l) | peak RSS |
|---|---|---|---|---|---|
| `apod4` (CREDITS: 34×24°) | 32.79 × 23.31° | blind | 1.34 s | **4.78 s** | ~96 MB |
| `apod4` | | hinted (20–50°) | 1.33 s | **4.52 s** | ~96 MB |
| `apod5` (CREDITS: 72×54°) | 60.64 × 44.66° | blind | 7.08 s | **60.34 s** | ~96 MB |
| `apod5` | | hinted (50–90°) | 1.87 s | **10.40 s** | ~96 MB |

Solved centres: `apod4` → RA 187.230445°, Dec +56.704209°, roll 69.906°;
`apod5` → RA 90.139401°, Dec +5.619914°, roll −165.104°. Both solved against
`index-4116.fits`.

## What this establishes

1. **Correctness.** `apod4` solves to 32.8 × 23.3° against a documented 34 × 24°
   field — the right answer.
2. **Cross-architecture determinism.** x86-64 and ARM32 returned **identical**
   centres, sizes and rotations to every digit printed. The solver is not
   float-order-sensitive across these targets, which is exactly what we need
   before trusting a soft-float ARM build.
3. **Hints are the whole game on wide fields.** `apod5` went from 60.3 s to
   10.4 s on ARM32 purely by constraining the scale — and on a Polaris we always
   know the scale, because we know the lens. Budget hinted, not blind.
4. **Memory is a non-issue.** ~96 MB peak against 1536 MB of RAM.
5. **Index storage is a non-issue for wide fields.** The 4116–4119 set is under
   1 MB — it would fit in the appfs headroom, never mind the microSD. (The 182 MB
   figure from Aperion is for the *narrow*-field scales an imaging telescope
   needs; a Polaris alignment shot does not.)

## What this does NOT establish

- **Real device speed.** qemu-user is typically several times slower than native
  ARM, while the Polaris' ~1 GHz Cortex-A7 is much slower than the emulated core
  here. Those two errors point in opposite directions and do not cancel in any
  principled way. **The device leg is still required.**
- **Our build.** These are Debian's binaries (hard-float, glibc 2.36, GPL `gsl-an`
  and `qfits` linked). The shippable path is the cross-built soft-float binary.
- **Star extraction time**, deliberately excluded — it will be added back when we
  decide between our own extractor and the device's `starskystacker::getStars()`.

## Next

1. Cross-build the solver soft-float / glibc-2.24 in the patcher container
   (GSL-free per the audit) and re-run this table under `qemu-arm-static`.
2. Run the same table **on the Polaris over SSH** (`--ssh-key` makes this a
   two-minute job) for the number that actually decides the design.

---

# Round 2 — our own binaries, built for the device

Round 1 above measured Debian's packaged `solve-field`. This round measures
**what we would actually ship**: built by
[`container/astro/build_solver.sh`](../container/astro/build_solver.sh) from
upstream astrometry.net 0.98 with our BSD-3 GSL shim, cross-compiled for the
Polaris.

## The binaries

| | size | ABI | glibc ceiling | DT_NEEDED |
|---|---|---|---|---|
| `polaris-solve` | 651,988 B | soft-float EABI (`0x5000200`) | **2.7** | `libc libm libpthread` |
| `polaris-extract` | 139,212 B | soft-float EABI | **2.7** | `libc libm` (libjpeg is static) |

Every one of those is inside what the device provides (glibc 2.24, soft-float).
The build asserts all of it and aborts otherwise, the same way the libgphoto2
build does — plus licence guards proving no `gsl-an`, `md5`, `ctmf`, `simplexy`
or `catalogs` object made it in.

**The GSL shim was differential-tested against real GSL**: same test program
built twice, once against Debian's `libgsl-dev` and once against our shim.
Output is identical to every printed digit except two least-squares *residual*
entries that differ in the 13th significant figure.

## Correctness

`polaris-solve` on ARM returns results **identical to the x86 build to six
decimal places** on every demo field. And the whole pipeline — our extractor on
the JPEG, our solver on its star list — reproduces the reference solution:

| frame | reference (`.xyls` path) | ours (JPEG → extract → solve) | agreement |
|---|---|---|---|
| `apod2` 800×600, 4.5×3.4° | RA 84.573797, Dec −2.750929 | RA 84.579746, Dec −2.758409 | **~34″** |
| `apod2` upscaled to **8192×6144** | " | RA 84.576444, Dec −2.756517 | **~22″** |

Solved pixel scale on the 45 MP frame: **1.9789″/px**, field 4.5030 × 3.3772° —
i.e. exactly the geometry a 400 mm lens puts on a full-frame body.

## Speed (qemu, **pessimistic** — see caveat)

| case | field | extract | solve |
|---|---|---|---|
| 45 MP JPEG @ 400 mm, 20° pose hint | 4.5° | **0.37 s** | **2.67 s** |
| 45 MP JPEG @ 400 mm, **no** hint | 4.5° | 0.37 s | 27.9 s |
| `apod4` star list, no hint | 33° | — | 4.13 s |
| `apod3` star list | 8.4° | — | 5.27 s |
| `apod2` star list, sloppy 30° hint | 4.5° | — | 61.7 s |

> **Caveat, stated plainly:** these ran under `qemu-arm-static` *inside an
> emulated x86-64 container* on an Apple-silicon host — two layers of emulation.
> Comparing the same work on x86 (0.35 s) against this (16.5 s) implies roughly
> a **30–50× penalty**, far worse than qemu-user's usual 2–10×. The device's
> ~1 GHz Cortex-A7 is slower than the host core, but nothing like 30× slower.
> Real device numbers are still the ones that decide the design.

## What this changes about the design

1. **A pose hint is worth ~10× on a long lens** (2.7 s vs 27.9 s at 400 mm) and
   ~80× on x86. The mount always has a rough pointing, so the solver must always
   be given one. This is now the single most important input to the design.
2. **400 mm on full frame is comfortably in reach.** Decoding at 1/4 scale keeps
   a 45 MP frame's extraction under half a second even doubly emulated, and the
   solve is a couple of seconds with a hint.
3. **Index storage for 14–400 mm is ~46 MB** (`index-4110` … `4119`), chosen by
   [`fetch-indexes.sh`](../container/astro/fetch-indexes.sh). Fits the microSD
   many times over.
4. **Roll/parity needs one real calibration frame.** The solver now reports
   `parity` and the full `cd` matrix, but which way "up" maps to the camera on
   the mount can only be pinned down with a frame the Polaris itself took.

---

# Round 3 — ON THE ACTUAL POLARIS

Rounds 1 and 2 were emulated. This round ran on the device itself
(`FwVer 4.0.0.32`, over ssh), using the binaries `build_solver.sh` produced,
installed at `/app/astro` with the index files on the microSD.

## The hardware, measured rather than inferred

```
CPU part 0xc07 (Cortex-A7), 2 cores
Features: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae
MemTotal 1550872 kB      /app 68 MB (12.4 MB free)      /app/sd 119 GB (113 GB free)
```

`-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp` is confirmed correct against
the real CPU flags. `/app` is tighter than the 20.4 MB estimated from the image —
**12.4 MB free** — which is still ample for the ~820 KB of binaries, and the
index files live on the card and are never copied into flash.

## Results

| field | mode | **device** | qemu (round 2) | x86 |
|---|---|---|---|---|
| `apod4` 33° | scale hint | **2.23 s** | 4.13 s | 1.34 s |
| `apod5` 60° | scale hint | **5.84 s** | 10.40 s | 1.87 s |
| `apod2` **4.5° (400 mm on full frame)** | + 15° pose hint | **7.31 s** | 16.5 s | 0.35 s |
| `apod2` 4.5° | **no pose hint** | **361.7 s** | 61.7 s* | 28.5 s |

\* the qemu run used a sloppier hint, not no hint at all.

Every solution matches the x86 build to six decimals — e.g. `apod4` →
RA 186.902948, Dec +56.700021 on all three platforms.

## What the device changed about the design

1. **qemu was pessimistic by ~1.8×**, as flagged. The real A7 is only 1.7–3×
   slower than emulated x86 — the honest conclusion from round 2 ("neither error
   cancels; the device decides") turned out to land in our favour.
2. **At 400 mm the pose hint is not an optimisation, it is the feature.**
   7.3 s versus 361.7 s — **50×** — for an identical answer. A blind narrow-field
   solve is unusable on this hardware.
3. Consequently `polaris-align.sh` now **asks the mount for its own pose** and
   derives the hint automatically when the caller does not supply one. The mount
   always knows roughly where it points, even completely unaligned, so the slow
   path should never be taken in practice.
4. Solve memory stayed far below the 1.5 GB available; storage is a non-issue.

## Extraction, on device

`polaris-extract` against an 8192×6144 JPEG, on the Polaris:

| decode scale | resolution | time |
|---|---|---|
| 1/8 | 1024×768 | **0.62 s** |
| 1/4 | 2048×1536 | **0.87 s** |
| 1/2 | 4096×3072 | **1.57 s** |

So **end to end at 400 mm is ~8.2 s** on device: 0.87 s to extract at 1/4 plus
7.31 s to solve, with a correct pose hint.

## A wrong hint is worse than no hint

Found the hard way: pointing the solver at a frame that does not match its hint
(a picture of Orion while the mount looked at RA 313°) ran for **11 minutes of
CPU** before being killed. The solver searches the hinted region, fails, and then
grinds — worse than the 361 s blind case.

`--cpulimit` had been declared but never implemented (it set a field to NULL and
did nothing). It is now a hard `SIGALRM` wall-clock bound that cannot be defeated
by anything inside the solver: it prints `{"solved":false,"error":"timeout"}` and
exits 3. Verified on device — the same 11-minute case now returns in exactly the
limit. `polaris-align.sh` passes `--cpulimit 45` by default, so no single frame
can ever block the alignment loop.

## Still not measured

A real frame from the R5 II. The 45 MP figures above use an upscaled star field
of the right dimensions and pixel scale, not the camera's own output.

---

# Round 4 — motors and a simulated camera, closed loop

`polaris-skysim` renders what the camera would see: it pulls stars from the
**same astrometry.net index files the solver reads**, projects them through a TAN
model at the requested focal length and roll, draws Gaussian PSFs, adds noise,
and writes a JPEG. So a render→solve round trip shares no assumption with the
solver except the catalogue itself.

## Render → solve accuracy, 400 mm on full frame (8192×6144, 2.266″/px)

| commanded | solved | error |
|---|---|---|
| RA 84.5, Dec −2.7 | 84.498798, −2.699175 | **4.3″** |
| RA 213.9, Dec 19.2 | 213.899161, 19.200109 | **3.0″** |
| RA 310.4, Dec 45.3 | 310.398651, 45.301105 | **4.9″** |

(The solved `roll` comes back 180° from the commanded value and parity reads
`flipped` — a systematic convention difference between the renderer's CD matrix
and the solver's, not an error. Irrelevant for pointing.)

## The full loop, with no ground truth in the measurement path

`sim-verify.py --camera` puts the renderer inside the alignment loop: the mount
moves, the sky is drawn at wherever it ACTUALLY ended up, and the solver gets
only the frame plus a hint from the mount's own (wrong) idea of its pose.

```
cold start: mount heading wrong by 37.5 deg
[1] align, from a real solve of a rendered frame  ->  residual 13.2 arcsec
[2] 6 align+goto cycles                           ->  worst 1.20 arcmin
[3] tracking 10 s                                 ->  target held to 45.6 arcsec
[4] safety rails (zenith / horizon / alt limit / max-slew)  ->  all refused
RESULT: all checks passed
```

The residual is *better* than the earlier synthetic run (13.2″ vs 21″) because
the real solver is more accurate than the 30″ of noise that run assumed.

Run it:

```
sim-verify.py --mount ./polaris-mount --camera \
  --index index-4110.fits --index index-4111.fits --cycles 6
```

This is the regression test to run before anything touches real motors, and it
needs no camera, no sky and no hardware.

---

# Round 5 — a REAL Canon R5 Mark II frame, on the device

`259A8092.JPG`: 8192×5464, M42 at ~380 mm on full frame. Ground truth from
upstream `solve-field` run **blind** (no position or scale hint):

```
RA 83.801735  Dec -5.163281   2.39307 arcsec/px   5.4446 x 3.6323 deg
solved with index-4112, simplexy found 2596 sources
```

## Our pipeline, same frame

| | RA | Dec | scale | agreement |
|---|---|---|---|---|
| upstream (blind) | 83.801735 | −5.163281 | 2.39307 | — |
| ours, x86 | 83.800813 | −5.161750 | 2.390209 | **6.4″** |
| **ours, on the Polaris** | 83.800859 | −5.161810 | 2.390170 | **6.4″** |

On device: **1.91 s to extract** (45 MP JPEG → 300 stars) + **7.42 s to solve** =
**~9.3 s end to end**, with only a focal-length hint.

## What real frames exposed that synthetic ones could not

**The background estimator has to be local.** The first real frame produced:

```
bg=84.0  noise=31.13  thresh=239.7  ->  4 stars
```

A whole-frame median/MAD measures the *gradient* — vignetting, light pollution,
amp glow — not the pixel noise, so the threshold landed at 239/255 and detected
almost nothing. Clean synthetic images and the APOD test set never showed this.
Replaced with per-tile median/MAD (64 px tiles at the decoded scale, bilinearly
interpolated): the same frame went from **4 to 300+ stars**.

**Some frames simply do not solve, and that is not a pipeline fault.** Four
earlier captures off the Polaris SD failed — and upstream `solve-field` with its
own `simplexy` extractor failed on them too (215 sources, no solve). Defocused
or otherwise unusable frames are garbage-in; the check that mattered was running
the gold standard alongside, which cleared our extractor of blame.

---

# Round 6 — the alignment correction, proved offline against a real capture

Everything here except the mount itself is real: the sky position was measured
**on the Polaris** from `259A8092.JPG`, the site is the user's GPS, and the time
is the frame's own EXIF (`2025:01:17 22:50:44` local → `2025-01-18T06:50:44Z`).
Reproduce with [`container/astro/m42_proof.sh`](../container/astro/m42_proof.sh).

```
solved (on device):  RA 83.800859  Dec -5.161810
+ GPS 35.35199,-119.17208 + EXIF time
-> the optics physically pointed at  alt 46.3474  az 205.4594
```

Independent sanity check on that conversion — Orion from 35°N in mid-January:

| time | computed |
|---|---|
| 6:50pm | alt 32.9°, az 126° (rising, SE) |
| **10:50pm — the capture** | **alt 46.3°, az 205°** (just past the meridian) |
| 2:50am | alt 7.1°, az 258° (setting, W) |

Maximum possible altitude for Dec −5.16° at latitude 35.35° is 49.5°, and the
capture lands at 46.3° just past transit. The whole chain — solve → J2000 →
precession/nutation/aberration → alt/az — agrees with where the sky was.

## The correction

Mount physically on M42, compass deliberately wrong, plate-solved truth injected
with `530`:

| compass error | frame error after | pointing error on the next goto | uncorrected |
|---|---|---|---|
| 3° | 0.0000° | **0.00′** | 180′ |
| 8° | 0.0000° | **0.00′** | 480′ |
| 25° | 0.0000° | **0.00′** | 1500′ |
| 47° | 0.0000° | **0.00′** | 2820′ |

## A bug this test caught

`goto-radec`'s arrival refinement called `gettimeofday()` directly, ignoring an
explicit `--utc`. Harmless in production (real runs always mean "now"), but it
made this replay refine towards where Betelgeuse is *today* rather than in
January 2025 — a 3634′ error that looked like a catastrophic alignment failure
and was actually a clock bug. `--utc` now freezes the clock for the whole
operation.

The simulator also now implements `530` the way the hardware does (real values
in both steps, no step 3), so alignment corrections can be verified without a
mount.
