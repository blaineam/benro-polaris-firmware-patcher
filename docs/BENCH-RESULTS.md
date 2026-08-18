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

## Still not measured

Star extraction from a real 45 MP camera JPEG, on device. Everything above feeds
the solver pre-extracted star lists, which isolates the matching algorithm —
`polaris-extract` timing on device needs a real frame from the R5 II.
