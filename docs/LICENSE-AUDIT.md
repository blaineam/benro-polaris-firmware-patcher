# Licence audit — on-device plate solving

**Scope:** everything that would end up on a Polaris if we build an
astrometry.net-based solver into the firmware. Audited against the
**astrometry.net 0.98** tree (upstream commit `1398028b`, as vendored by Aperion),
its `CREDITS`, `LICENSE`, and the per-file headers.

**This is an engineering audit, not legal advice.** It records what the source
files actually say, so the build can be designed around them.

---

## Verdict

**You cannot make an astrometry.net-based solver permissively licensed just by
replacing GSL.** `gsl-an` was one of *four* copyleft components, and the FITS
layer — which the solver genuinely needs in order to read index files — is GPL.

**But the Polaris build is safe anyway, via one rule:**

> ### The solver is its OWN PROCESS. Nothing GPL is ever linked into `pgphoto`, `polestar_app`, or our MIT loader.

With that boundary the solver binary is distributed under **GPL v2-or-later**
(v3-or-later if we also use astrometry.net's star extractor), our patcher stays
MIT, Benro's binaries are untouched, and compliance is the same source-offer
mechanic the tool already performs for LGPL libgphoto2.

---

## Component inventory

| Component | Licence (from the source itself) | Needed to solve? | Verdict |
|---|---|---|---|
| `solver/`, `util/`, `libkd/` — astrometry.net's own code | **BSD-3** (`LICENSE`, `libkd/LICENSE`) | **yes** | fine |
| `gsl-an/` — GSL 1.11 | **GPL v3+** (`CREDITS`, `gsl-an/COPYING`) | yes (7 routines) | **removable** — Aperion's BSD-3 `gsl-shim` already replaces it |
| `qfits-an/` — qfits 6.2 + AN's `anqfits.c` | **GPL v2+** — `qfits-an/NOTE-Astrometry.net`: *"we hereby claim all our modifications … and release our modifications under the GPL v2 or later"*; `anqfits.c` header: *"Licensed under GPL v2 or later."* 14 of 15 `.c` files carry the note | **yes** — index/FITS reading | **the real trap** |
| `util/ctmf.c` (+ `solver/an_mm_malloc.h`) | **GPL v3+** (`CREDITS`) | only via `simplexy` | **avoidable** — see rule 3 |
| `util/bt.c` (GNU libavl) | **GPL** (`CREDITS`) | **no** — only `solver/hpquads.c` (index *building*) | not in closure |
| `util/md5.c` | **GPL v2+** (`CREDITS`) | no other file in the solve closure calls it | not in closure |
| `catalogs/brightstars*` (Stellarium-derived) | **GPL v2+** (`CREDITS`) | no — labelling, not solving | **do not link `libcatalogs`** |
| `catalogs/NGC.csv` (OpenNGC) | **CC-BY-SA 4.0** | no | same |
| cfitsio | NASA/HEASARC permissive | yes (already a dep) | fine |
| our `gsl-shim` port | **BSD-3** (ours) | yes | fine |

### Upstream's own words

astrometry.net's `LICENSE` leads with BSD-3 for the team's code, then says the
whole work must go out under GPL v3+ because of what it bundles. Their published
README is blunter still: *"The Astrometry.net code suite is free software
licensed under the GNU GPL, version 2."* **Treat the suite as GPL by default.**

### How big is the FITS trap, exactly

Measured on the *solve-only* closure (`solver/solver.c`, `onefield.c`, `engine.c`,
`verify.c`, `tweak2.c`, `util/index.c`, `quadfile.c`, `starkd.c`, `codekd.c`,
`fitsbin.c`, `fitstable.c`, `fitsioutils.c`, `fitsfile.c`, `sip_qfits.c`,
`xylist.c`, `matchfile.c`):

- **13** distinct `anqfits_*` entry points
- **44** distinct `qfits_*` entry points
- `solver/solver.c` itself — the actual matching algorithm — **touches neither**.
  The taint is entirely in the file-I/O layer.

So a cfitsio-backed "qfits-shim" (same trick as the gsl-shim) is ~57 entry
points. Tractable, but multiples of the gsl-shim's 7. **We do not need it for
the Polaris** — only for in-process linking or an App Store build (below).

---

## The rules we build to

1. **Process boundary.** The solver ships as a standalone executable
   (`polaris-solve` / `polarissolved`). Communication with anything Benro-owned
   or MIT-owned is **files, sockets, or a message queue — never linking.**
2. **The in-app hook (phase 4) marshals, it does not link.** Our
   `SP_OneStarCal` hook stays MIT and merely hands a request to the solver
   process and reads the answer back. No GPL object ever enters `polestar_app`'s
   address space.
3. **Prefer feeding a star list over `simplexy`.** If star extraction happens in
   our own code (or via the device's existing `starskystacker::getStars`), the
   GPLv3 `ctmf.c` never enters the build and the solver binary stays at
   **GPL v2-or-later**. astrometry.net's own `demo/*.xyls` files prove the
   star-list-in path is a first-class interface.
4. **Do not link `libcatalogs`.** It carries GPLv2+ Stellarium-derived data and
   CC-BY-SA data, and solving does not need it.
5. **Keep `gsl-an` out** (port Aperion's BSD-3 shim). Not because it changes the
   GPL answer, but because it drags the binary from GPLv2+ up to GPLv3+ and adds
   a dependency we do not need.
6. **Ship no index files, and mirror none.** The patcher may *fetch and verify*
   them onto the user's microSD on request. The **4100 series (Tycho-2, scales
   7–19)** is what upstream recommends for fields wider than 1° — our regime —
   and the widest scales are tiny (`index-4119` is 144 KB).
7. **Ship no test images.** astrometry.net's `demo/apod*.jpg` are **copyrighted
   APOD images** (Russell Croman, SSRO, Noel Carboni, et al. — see `demo/CREDITS`).
   Fine for local benchmarking, never committed or redistributed.
8. **Source offer.** The patcher already writes `out/licenses/` for LGPL
   libgphoto2; it must do the same for the solver: upstream tarball URL + exact
   commit, our patch set (which lives in this repo), and the GPL text. Because
   the user builds the image themselves with this public tool and flashes it
   themselves via Benro's own SD-card procedure, GPLv3 §6 "Installation
   Information" is satisfied by construction — there is no lock-out to defeat.
9. **This repo stays MIT** and keeps shipping no binaries: the solver is built
   from upstream source inside the container at run time, exactly like
   libgphoto2 is today.

---

## If we ever need a permissive solver

Only two things need it: linking the solver *into* another process, or shipping
it in an App Store binary. The work is then:

1. `gsl-shim` — **done** (Aperion's, 713 lines, BSD-3; needs an ARM/Linux backend
   with no BLAS, since astrometry.net only uses 3×3 / 4×4 / N×3 matrices).
2. `qfits-shim` — **new**, ~57 entry points over cfitsio.
3. Own star extractor instead of `simplexy` (drops `ctmf`).
4. Don't link `libcatalogs`.

Result: BSD-3 (astrometry.net) + permissive (cfitsio) + BSD-3 (our two shims).

---

## Advisory: this affects Aperion today

Aperion's `scripts/astrometry/bundle-xcframework.sh` combines, per slice:

```
libastrometry.a libanfiles.a libanutils.a libanbase.a libkd.a
libcatalogs.a  libgslshim.a  libqfits.a  libcfitsio.a
```

The `gsl-shim` correctly removed the GPLv3 `libgsl-an.a` — but **`libqfits.a`
(GPL v2+) and `libcatalogs.a` (GPLv2+ Stellarium-derived `brightstars`) are still
in the link**, inside a closed-source App Store binary. GPLv2 has no App Store
exception. If `simplexy` is reachable from `AstrometrySolver.solve()`, GPLv3
`ctmf.c` is in there too.

Same class of problem the gsl-shim was written to solve — just not finished. The
fix is items 2–4 above, and doing it once serves both projects. **Worth checking
before the next submission.**

---

## Open items

- [ ] Confirm nothing in the final link pulls `qfits_md5` / `util/md5.c`.
- [ ] Confirm the exact index-file terms from upstream's `GETTING-INDEXES` (the
      file their README references is not in the 0.98 tree and 404s on the doc
      site) before *any* mirroring — rule 6 keeps us clear meanwhile.
- [ ] Record the chosen upstream commit + our patch set in `out/licenses/` when
      Phase 1 lands.
