# On-device plate solving for the Benro Polaris

Astrometric plate solving, automatic alignment, auto-guiding, a web UI and an
ASCOM Alpaca telescope endpoint — all running on the Polaris itself, with no
laptop in the field.

> **Status: ALPHA.** Every number below was measured on hardware or in a
> closed-loop simulation, and the gaps are listed explicitly in
> [What is and is not verified](#what-is-and-is-not-verified). Read that section
> before trusting this on a night you care about.

---

## What it does

1. You open astro mode and do the app's compass alignment.
2. You goto a target.
3. Before you tap confirm, the daemon plate-solves what the camera is actually
   looking at, corrects the mount's heading, slews so the target is genuinely
   centred, verifies that with a second solve, and confirms for you.
4. During the session it watches for tracking drift and corrects it.

The point is that step 3 replaces *"nudge the mount until the star looks
centred"* with a measurement.

---

## Requirements

| | |
|---|---|
| Firmware | FwVer 4.0.0.32, patched with this tool |
| Camera | Canon EOS R5 Mark II (the only body tested) |
| microSD | ~50 MB free for index files |
| Focal length | 14–400 mm covered by the shipped indexes |

---

## Install

```sh
./build-astro.sh --out out/astro
```

Copy the **contents** of `out/astro/COPY-TO-SD-CARD-ROOT/` to the root of the
Polaris' microSD card:

```
<SD root>/astrometry/index-41xx.fits      index files (~44 MB)
<SD root>/polaris-astro/                  binaries and scripts
<SD root>/polaris-astro/site.conf         your configuration
```

Then create `site.conf` (see below) and reboot. The boot hook installs the
binaries to `/app/astro` and starts whatever you enabled.

---

## Configuration — `/app/sd/polaris-astro/site.conf`

```sh
# REQUIRED. Without these the solver has no hint and Alpaca reports the wrong
# site. Decimal degrees, east/north positive.
LAT=35.35199
LON=-119.17208
FOCAL=400

# Web UI + Alpaca on :8090.  (8080 is pgphoto's own MJPG stream; 80, 8081,
# 9090 and 22 are also taken.)
POLARIS_HTTPD_PORT=8090

# Auto-solve during the app's calibration.
AUTOSOLVE=1
AUTOSOLVE_DRY_RUN=1      # 1 = log what it WOULD do, touch nothing. Start here.
MIN_LOGODDS=100          # solve-quality gates; below these it does nothing
MIN_MATCHES=12
CENTRE_TOL_DEG=0.15      # how well centred before it will confirm (~9 arcmin)
MAX_ALIGN_ALT=65         # refuse to derive a heading above this altitude

# Auto-guiding after a successful alignment.
GUIDE=0
GUIDE_DRY_RUN=1
GUIDE_INTERVAL=30        # seconds between drift checks
GUIDE_THRESH=60          # correct once drift exceeds this many arcsec

# Clock. The Polaris runs LOCAL time while reporting it as UTC; "auto" reads
# the real offset from the app's own 782 message. Only override if you know why.
TZ_OFFSET_SEC=auto

# Record the app<->device protocol to /app/sd/align-flow.log (debugging only).
CAPTURE_ALIGN_FLOW=0
```

**Both dry-run flags default to 1 on purpose.** Armed, this writes a heading
correction and an alignment confirm to a live mount. Run it dry once, read
`/app/sd/polaris-autosolve.log`, and only then set them to 0.

---

## Capture: images land on BOTH cards

The patcher configures capture so a frame is written to the **camera's own
memory card** *and* downloaded to the **Polaris microSD**. The RAW stays on the
camera; only the JPEG (~9.4 MB) crosses the wire, which is all the solver needs.

This required fixing two upstream libgphoto2 bugs — see
[UPSTREAM-LIBGPHOTO2-BUGS.md](UPSTREAM-LIBGPHOTO2-BUGS.md) and
[CAPTURE-PATH.md](CAPTURE-PATH.md). Measured announcement latency is ~0.94 s
against pgphoto's 7.03 s budget.

---

## Web UI

`http://<polaris ip>:8090/` — solve status, last solution (RA/Dec, roll, pixel
scale, field size, match count, solve time), a live log tail, and buttons to
solve the latest frame or wait for the next one.

**The page never polls the mount.** Every connection to the control port is seen
by the device as an app connecting, and polling made the Benro app re-prompt for
compass calibration continuously. Mount state is read passively from the
device's own log; an active read happens only when you press **Read Mount**, and
only when the mount is already aligned.

---

## ASCOM Alpaca

Point Stellarium, NINA or SkySafari at `<polaris ip>:8090`, device 0.

```
/management/v1/configureddevices          discovery
/api/v1/telescope/0/rightascension        live position (HOURS, per spec)
/api/v1/telescope/0/declination           degrees
/api/v1/telescope/0/slewtocoordinates     -> goto
/api/v1/telescope/0/synctocoordinates     -> plate-solve align
```

35/35 conformance checks pass; run them yourself with:

```sh
python3 tests/alpaca_conformance.py http://<polaris ip>:8090
```

---

## What is and is not verified

### Verified on hardware

| | |
|---|---|
| Capture to camera card **and** Polaris | JPEG ~9.4 MB, ~0.94 s announce |
| Solver at live-view resolution (960×640) | 20–40″ over four fields, 1–4 s |
| Real R5 II frame solved on device | 6.4″ vs upstream, 9.3 s |
| Alpaca conformance | 35/35 |
| Boot autostart | across real reboots |
| Mount untouched while unaligned | 0 connections measured |

### Verified in closed-loop simulation

| | |
|---|---|
| Correction arithmetic | injected 5.000° → recovered 5.007804° |
| Full alignment loop, real motor commands | 37.5° error → 0.122° in one pass |
| Drift measurement | 0–0.4% error over 20″–9000″ |
| Guiding loop, real motor commands | drift held <61″ vs 408″ unguided |

### NOT verified

- **Real stars.** Everything above used rendered fields or daylight frames. The
  geometry is proven at live-view resolution; whether your sky gives enough
  signal in a short live-view frame is not something this repo can answer.
- **The real `530 step:1` trigger.** The daemon's detection has only ever been
  driven by log lines written by the test, not by an actual calibration.
- **Any camera other than the R5 Mark II**, and any firmware other than 4.0.0.32.

### Known limitations

- **Field rotation is not modelled** by the drift matcher. On an alt-az mount it
  will eventually degrade matching, at which point the guider re-anchors with a
  full solve rather than correcting wrongly. That is the intended failure mode,
  not a silent one.
- **Corrections are small slews, not rate adjustments.** No axis-rate primitive
  is exposed by the mount, so a guide correction is a `goto-radec`.
- **Live view is 960×640** and that is the ceiling: Canon exposes no live-view
  size control through libgphoto2 2.5.34 (`liveviewsize` is a Nikon property).

---

## Troubleshooting

**The app keeps asking for compass calibration.**
Something is connecting to the control port while the mount is unaligned. Check
nothing is polling it: `ps | grep polaris-`. The shipped code only reads the
mount on an explicit request and only when aligned.

**Solves fail with a hint but succeed blind.**
The hint is wrong by more than the search radius. `HINT_RADIUS` defaults to 60°
because the hint comes from the compass, which is exactly what is being
corrected. A too-narrow hint fails *slowly*: measured 120 s to fail at 20°,
versus 3 s to succeed blind.

**Coordinates are wildly wrong (tens of degrees).**
The device clock. It runs local time while reporting UTC; seven hours is ~105°
of hour angle. `TZ_OFFSET_SEC=auto` reads the true offset from the app's `782`
message. Check the daemon's first log line, which prints the clock it resolved.

**It solved but refused to confirm.**
By design: it verifies centring with a second solve and, if the residual never
comes inside `CENTRE_TOL_DEG`, applies the heading correction but leaves the
dialog for you. A wrong alignment is worse than no alignment.

---

## Testing without a sky

```sh
# render a field, solve it back, no hardware at all
polaris-skysim --index ... --ra 83.8 --dec -5.2 --out /tmp/f.jpg
polaris-extract --jpeg /tmp/f.jpg --downsample 1 > /tmp/s.txt
polaris-solve --index ... --stars /tmp/s.txt --width 960 --height 640

# drive the whole alignment loop against a simulated mount
python3 container/astro/polaris-sim.py --bind 0.0.0.0 --port 9099 \
    --status-port 9098 --az-error 37.5 --lat .. --lon .. --track-drift 120
```

Then point the daemon at it with `MOUNT_HOST`, `MOUNT_PORT`, `SIM_STATUS` and
`CAPTURE_MODE=render`. The sim reports where it is *really* pointing, so the
daemon can be given a rendered view of that and the loop closes with no sky.

The simulator's own coordinates sit ~0.2° from `polaris-mount`'s because it does
not apply precession; compare sim-to-sim or mount-to-mount, not across.

---

## Files

| | |
|---|---|
| `polaris-solve` | astrometry.net front end (GPL, separate process) |
| `polaris-extract` | JPEG → star list |
| `polaris-match` | frame-to-frame drift, for guiding |
| `polaris-skysim` | renders a star field, for testing |
| `polaris-mount` | protocol client |
| `polaris-httpd` | web UI + Alpaca |
| `polaris-logwatch` | truncation-safe log follower |
| `polaris-autosolve.sh` | the calibration daemon |
| `polaris-guide.sh` | the guider |
| `solve-now.sh` | one-shot solve from the command line |

Licensing: `polaris-solve` is built from astrometry.net and is GPL; it runs as
its own process and nothing GPL is linked into `pgphoto` or the MIT loader. See
[LICENSE-AUDIT.md](LICENSE-AUDIT.md).
