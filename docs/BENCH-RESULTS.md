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
