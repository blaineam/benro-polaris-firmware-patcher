# Changelog

## Unreleased (branch `astro-plate-solving`) — solver built, motors untouched

### Added — a web control app that replaces the phone

- **`polaris-httpd` now serves the Benro Connect control surface at `/`**: live
  view, pan/tilt/rotation jog with speed gears and re-centre, the camera's own
  exposure lists (shutter/aperture/ISO/EV/WB), shutter and record, battery and
  card status, and the Wi-Fi settings — on any browser, with no phone. 41 KB of
  source, **13 KB embedded** in the binary (gzipped at build time, served with
  `Content-Encoding`), so it costs almost nothing on the SD card. The existing
  solver dashboard is unchanged and moved to `/legacy`.
- **`polaris-link`** — one long-lived registered connection to the head, shared
  by the page, the solver and the Alpaca bridge, replacing a `polaris-mount`
  fork per request. Telemetry at ~30 Hz instead of a five-second cache; a
  joystick is not buildable on the old path. It is also the radio keepalive, so
  `wifi-keepalive.sh` is redundant while the server runs.
- **`polaris-jog` — the dead man lives on the server.** Fast jog must be
  re-sent every 50 ms and nobody has established that the head stops when the
  stream stops; slow jog latches until explicitly released. So the browser only
  declares an intent with a 400 ms lease and the server owns both the repeat and
  the stop: a locked phone, a closed tab or a Wi-Fi drop mid-slew **stops** the
  head instead of leaving it running.
- **An immersive, preview-first control screen with analog joysticks.** The
  Control tab was rebuilt to compose the way the phone app does — a full-bleed
  live-view **stage** with everything floating on top: two **draggable analog
  joysticks** under both thumbs (pan/tilt left, rotation right), the shutter and
  camera settings below, status and stage buttons in the corners. The sticks are
  **velocity** controls — displacement sets direction _and_ speed (push to the
  edge = full speed), the speed slider caps the top speed, the knob turns red
  while moving, and a **double-tap re-centres that axis** (523), as in the app.
  They ride the exact continuous fast-jog path the server already owned
  (`speed:<±100..2500>`), so the 400 ms lease and the server dead-man cover them
  unchanged: let go, lock the phone or close the tab and the head stops. Every
  input — sticks, the arrow pads (now a tucked-away "precise nudge"), the arrow
  keys — funnels through one `moveAxis()` sender, so a live speed change and the
  lease keepalive can never double-drive the wire.
- **Built for low-vision use at the scope.** A size control on the stage toggles
  Normal ↔ Large, scaling the preview, the joysticks and the text together (the
  whole reason to control a mount from a browser instead of a phone), and the
  choice persists. A fullscreen button fills the screen with the live view
  **while keeping the joysticks overlaid**, so you can frame and jog on a big
  image at once.
- **Tilt compensation (538) and auto-level (549) in the Astro tab — track on an
  unlevel tripod.** Surfaces the head's own **`SP_SET_TILT_STATE`**, the setting
  the app buries three taps deep in a help sheet: with it on, the head corrects
  for its own tilt from its attitude sensor, so deep-sky tracking doesn't need a
  perfectly level tripod. It is a persistent setting, not motion, and the toggle
  reads the head's current state (537) on entry rather than guessing. Auto-level
  (549, which physically drives the head to level and so is confirm-gated and
  refused mid-track) sits beside it for when you do want a mechanical level.
- **`docs/APP-PROTOCOL.md`** (122 opcodes, every claim cited to the decompiled
  app) and **`docs/APP-FEATURES.md`** (the app's screens, programs and the
  traps in porting them).
- **Timelapse (272) and panorama (271) programmes.** Both run head-side; the
  server owns the progress poll (`polaris-prog`), so a run keeps going and keeps
  being watchable with no browser connected — the one capability the phone app
  structurally cannot have. The Programs tab starts them (two-tap
  arm-then-confirm, since both fire the shutter), shows live progress with an
  honest bar (a real fill when the count is known, indeterminate when unlimited,
  amber when the head goes quiet), and offers cancel plus panorama pause/resume
  and a live per-position interval. Timelapse shows derived shooting/video
  durations as explicit lower bounds. `test_prog.c`, 39 assertions on the wire.
- **Panorama's start payload is treated as inference, not fact** — its binding
  method failed to decompile — so `/api/prog/pano` without a confirm returns the
  exact frame that would be sent and the UI shows it, labelled as unverified,
  before anything reaches the motors.
- **HDR (280) and focus stacking (270).** Both payloads are read straight from
  the app's own `getStartShootingParameter()` builders — not inference. HDR is a
  three-frame shutter bracket around the current exposure: since the head speaks
  exposure INDICES, it walks the shutter index and the preview resolves the
  three indices back to the camera's real shutter labels ("1/250 / 1/60 / 1/15")
  so the bracket is never a mystery before it fires; it refuses to run until the
  shutter list has loaded, and the head pushes its progress rather than being
  polled. Focus stacking racks the lens between two marked limits (four
  hold-repeat rack buttons through the 311 focus-adjust, mark near/far, dry
  preview or start) and stitches all-in-focus; its start frame carries the
  genuine `;num:<shots>;` double semicolon. Both fold into the same run view,
  poll model and dead-man as timelapse/panorama.
- **ASTRO mode with browser compass alignment.** The Astro tab enters celestial
  mode (285 mode:8) and holds the AHRS attitude heartbeat alive server-side (the
  app re-arms it every 5 s; a browser tab can't be trusted to). Alignment offers
  three routes, best first: **solve the sky** (kick this repo's own solver with
  apply=1 — no compass, no phone sensors, more accurate than a magnetometer),
  **phone compass** (DeviceOrientation/webkitCompassHeading when the browser is
  on a handset), and **manual bearing** — all ending in the same 527 SP_SET_YAW
  the app sends, with the server's lat/lon. Tracking is 531 with sidereal/lunar
  rate and a full/half toggle (the half flag is inverted on the wire, handled).
  `polaris-astro.{c,h}`, `test_astro.c` (24 assertions).
- **FREE PROGRAM (283) — a keyframe move flown by hand.** Jog the head, capture
  its live 517 pose as a keyframe, repeat, then play; the head walks the path on
  its own and keeps going with no browser attached. Linear or hold interpolation,
  an optional photo track, a previewed timeline.
- **PATH-LAPSE (272 + waypoints) and SUN (277).** PATH-LAPSE is a moving
  timelapse: capture 2–8 head poses as waypoints, each carrying the frames to
  shoot on the leg reaching it, and the head glides the path firing as it goes.
  It rides the **same** 272 opcode as the static timelapse — each waypoint is a
  `step:2` point with a gimbal pose in radians and a cumulative arrival time, and
  `SEND_END` sums the legs into the authoritative frame count — so it shares the
  timelapse poll, run view and dead-man exactly. SUN schedules a sunrise/sunset
  lapse across a time window (277): the head does the solar GOTO itself and
  parks, pushing its own progress like HDR. The window is validated with the
  **browser's** clock, not the device's (the Polaris clock is not UTC), and the
  times are serialised as the app's `yyyy,MM,dd,HH,mm,ss` local wall-clock. Both
  payloads are read from the app's own builders, not inferred; both preview the
  exact frame before the two-tap confirm.
- **HOLY GRAIL (305) — the day-to-night exposure ramp.** The last one, and the
  odd one out: it is **not a run** but a configuration. It uploads a target
  brightness curve and, optionally, the three exposure-axis ranges the head
  follows *while a timelapse or path-lapse runs*, ramping exposure from daylight
  into night. The whole 305 batch fires the way the app's does when its sheet
  closes — `SET_GRAIL_MODEL`, the opt-in `SET_PRIORITY`/`SET_ISO`/`SET_F`/
  `SET_SHUTTER`, then the `nodeCnt;para:<Δmin>/<ev>` curve. Two honest caveats,
  both surfaced in the UI: the ramp **meters through an external Optical Matrix
  Sensor Module accessory**, so without it the head cannot ramp; and while the
  step→field shapes are read from the decompile, the axis-value encodings (F is
  `%.1f`, shutter a 268 index) and the priority codes are **inferred**, so every
  305 batch is previewed before it is sent, exactly like panorama. Axis ranges
  are picked from the camera's *own* lists (ISO with the app's Auto-filter and
  6400 cap), the curve anchor at offset 0 is mandatory, and the runtime
  brightness readback filters out the shared-slot SET acks so it never shows a
  stale value as a reading. **That is every programme the Benro Connect app
  exposes now wired** — timelapse, path-lapse, panorama, HDR, focus stack, sun,
  astro, the FREE PROGRAM timeline and the Holy Grail ramp.
- **A real heisenbug caught by ASan before it shipped:** `absorb_reply`'s
  per-kind `seen[]` array was sized `[PROG_FOCUS+1]` and indexed by a kind that
  grew past it — a global-buffer-overflow that corrupted an adjacent global. It
  nearly recurred the instant PATH-LAPSE and SUN were added past the old bound,
  so the enum now carries a `PROG_KIND_COUNT` sentinel and every per-kind array
  is sized from it — the whole class of bug is gone, not just the one instance.
  Every test binary also runs under AddressSanitizer + UBSan, clean.
- The mount simulator learned jog, re-centre, registration, battery, card, the
  camera option lists, the astro control opcodes (mode/AHRS/yaw/half/pose), and
  now runs every programme with a real countdown — timelapse and path-lapse off a
  shared 272 model (the authoritative total taken from `SEND_END`'s `photoCnt`),
  HDR and SUN via a pushed completion, and it acks the Holy Grail 305 config
  batch and answers its brightness poll — so the whole control loop, motion
  through every programme, is testable with no hardware attached. `test_prog.c`
  is 134 assertions.
- **The solver dashboard, folded into the app.** The Astro tab gained native
  panels for **plate solving** (capture/latest/apply/cancel, focal override, the
  solved RA/Dec and the solver log), **auto-guiding** (on/off with a drift
  sparkline and the guide log), and **mount status** (mode/aligned/tracking,
  alt-az on demand, live attitude) — all wired to the `/api/solve|apply|cancel|
  focal|guide|state` endpoints that used to be reachable only from `/legacy`.
- **Dithering (540).** `SP_SET_DITHER_STATE` is surfaced as a toggle beside
  guiding — it shifts the framing a hair between exposures so stacking averages
  out hot pixels; a persistent setting, not confirm-gated motion (the nudges
  happen between frames of a running capture), and the toggle reads the head's
  real flag (539) on entry.
- **Native INDI telescope on TCP 7624.** KStars/Ekos and PHD2 are INDI-native
  and LX200 cannot express track-state, sync-vs-slew, or an abortable async
  slew — so the server now speaks a hand-rolled minimal `INDI::Telescope`
  alongside Alpaca and LX200, the same dependency-free way the Alpaca device is
  hand-rolled (**no libindi** — its C++/cfitsio/libnova stack will not
  cross-compile on the debian:9 ARM toolchain). CONNECTION, DRIVER_INFO,
  ON_COORD_SET, EQUATORIAL_EOD_COORD (goto to slew async, align to sync),
  TELESCOPE_ABORT_MOTION, TELESCOPE_TRACK_STATE, GEOGRAPHIC_COORD and TIME_UTC,
  all mapped to the same `polaris-mount` commands; each client is a forked child
  and the pointing is pushed ~1 Hz. Verified end-to-end with a protocol client
  (all eight vectors defined, interface=1, SYNC/SLEW→Busy/ABORT/geo round-trip,
  periodic push). `--indi-port` (default 7624), and the three bridges are named
  in the app's Observatory-bridges card.
- **INDI: real track, park, pulse-guiding, and a camera.** `TELESCOPE_TRACK_STATE`
  drives `polaris-mount track on|off` (real sidereal); `TELESCOPE_PARK` stops the
  mount (tracking off + abort — the Polaris has no safe motorised stow); pulse
  guiding (`TELESCOPE_TIMED_GUIDE_NS/WE`) jogs the RA/Dec axis briefly (the mount
  is goto, not ST4, so PHD2/Ekos calibrate the rate). And a **second INDI device,
  `Benro Polaris Camera`** (CCD), so Ekos sees a mount *and* a camera: its
  `CCD_EXPOSURE` returns the head's live-view JPEG as the `CCD1` BLOB
  (base64-streamed to the socket) — the head refuses an externally-triggered
  shutter in astro mode, so a mode-independent capture comes from live view; a
  real, solvable frame for framing/EAA, not a raw light frame. Verified with a
  protocol client: two devices, CCD BLOB round-trips exactly.
- **A guided target workflow, end to end.** The Astro tab now closes the loop:
  enter astro (tilt compensation handles an unlevel tripod) → plate-solve align
  → **pick a target from a built-in catalogue** (Messier / NGC / IC highlights,
  bright alignment stars, and the **Moon + planets** computed live from a
  client-side low-precision ephemeris, searchable) → **Go to** it (a native GOTO that
  slews and tracks, using the same `goto-radec` the bridges use, alignment-gated
  and two-tap) → auto-guiding holds it → **start the in-head astro capture**
  (a 272 interval sequence — the head shoots and stacks itself). The capture step
  is honest about the one hardware catch: the Polaris can refuse an
  externally-started shutter while tracking, so the UI says to trigger the first
  shot in the Benro app if it doesn't begin.
- **The last head settings, and the Sun — both gated by their real hazard.** The
  Settings tab gained **camera-plate direction** (546, a benign re-orient,
  confirm-gated) and **restricted angle** (release the head's travel limits,
  542). Releasing the limits lets the head swing into its own Astro Kit or the
  tripod, so — matching the app — it is gated behind a **physical-attestation**:
  542 stays OUT of the generic send allowlist, a dedicated `/api/astro/limits`
  route is the only way to release it, and it refuses without `attest=1`
  (re-enforcing the limits is always safe). The **Sun** joins the target
  catalogue behind the same kind of gate — its GOTO is locked until you confirm a
  **solar filter is fitted**, because a bare sensor pointed at the Sun is
  destroyed in seconds.
- **ASCOM Alpaca Camera device**, so NINA gets the same live-view capture INDI
  already gives Ekos. `/api/v1/camera/0/*` exposes a monochrome camera whose
  `startexposure` grabs a live-view JPEG, decodes it to grayscale via a new
  `polaris-extract --gray-pgm`, and serves it as an `ImageArray` (Type Int32,
  Rank 2, column-major `[x][y]`); it is listed in `configureddevices` beside the
  telescope. Same honest limit as the INDI CCD — a solvable preview frame, not a
  raw light frame. Verified end-to-end (startexposure → imageready → imagearray,
  pixels transpose correctly).
- **Astro autofocus (HFR) — the flagship missing piece.** An automatic focus
  that optimises the stars, the way NINA/Ekos do it: sweep the focuser, score
  each frame by how tight the stars are, settle at the sharpest point. New
  `polaris-extract --focus-metric` computes each star's HFR (flux-weighted mean
  radius) during detection and prints one line to maximise — the star term
  (many, tight stars) multiplied by a whole-frame gradient sharpness so gross
  defocus can't fake a secondary peak. `autofocus.sh` runs the sweep, driving
  focus relatively (opcode 311) through the server's own registered link and
  grabbing frames from the new same-origin `/api/snapshot`; with no focuser
  read-out it sweeps one way and returns from that side to take up backlash.
  `/api/astro/autofocus` (GET status + V-curve, POST confirm=1 / stop=1) forks
  it. The Astro tab shows a Run/Stop card with a live focus curve, best-step
  marker, and HFR read-out. Metric verified monotonic on a real-JPEG defocus
  series; the sweep verified on the bench to settle on the true peak and return
  the focuser exactly there; closed-loop motion is hardware-validated later.
- **Manual-focus jog, a framing grid, focus peaking, and a live histogram** on
  the Control tab / live-view stage. The MF jog is a standalone 311 focus
  control for pulling stars to a hard point of light. The grid is a
  rule-of-thirds + centre-cross overlay. Focus peaking (a gradient-magnitude
  glow) and the histogram read pixels from `/api/snapshot` — a same-origin
  single-frame proxy, because the cross-origin `:8080` stream taints a canvas.
  Pixel math verified on a synthetic frame; overlays verified in-browser.
- **The Astro tab reads as a numbered guided flow** — ① Level ② Align ③ Track
  ④ Auto-focus ⑤ Go-to ⑥ Capture — in one centred column instead of a scrambled
  two-column masonry, with the always-on diagnostics/bridges cards folded into a
  collapsible. Programs' eight-segment chooser is grouped into "Over time" and
  "Sequences". Verified in-browser on mobile and desktop.
- **Panorama shot-position grid.** The pano panel now draws the sweep as a
  cols×rows grid — the planned layout with the start corner marked, and, while a
  panorama runs, each cell lighting up shot → shooting → to-go as the head works
  through it. The fill follows the head's traversal for the exposed paMode-0 grid
  (a serpentine from the start corner, or an outward spiral for a centre start),
  labelled as assumed while the count is the head's own. `prog_status_json` now
  emits `start_dir`. Traversal verified for all corners, centre spiral, and a
  360-cell grid.
- **Media gallery — browse the SD with real thumbnails.** The head has no
  thumbnail endpoint, so the phone app pulls whole ~9 MB frames to show a gallery;
  running beside the files we generate a small colour thumbnail per photo
  (`polaris-extract --thumb`, a 6000×4000 frame → a ~2 KB 320-px thumb) and cache
  it. New endpoints `/api/media/{list,thumb,full}` (new `--media-root`, default
  `/app/sd`): list walks the capture dirs newest-first, thumb serves the cached
  thumbnail, full streams the frame in 64 KB chunks. The `path` parameter is
  confined to a .jpg under a known capture category, character-whitelisted, and
  realpath-checked inside the root — nothing else on the SD is reachable (seven
  attack vectors verified refused). New **Gallery tab**: a thumbnail grid with
  category filters and a tap-for-full lightbox.
- **AF/MF toggle** in the exposure strip, like the app's `[ MF AF ]` chips —
  sends `262 SP_SET_FOCUS` (already allowlisted, no motion). MF is what the focus
  jog, focus stack and Astro auto-focus all need. The mode value is inferred
  (`mod:1`=MF, `mod:0`=AF) — flagged to confirm on hardware.
- **Runtime observing site** (Settings). New `/api/site` GET/POST sets the
  latitude/longitude the solver hint, Alpaca and every alt/az conversion use, at
  runtime, and persists to `site.conf` (rewriting only `LAT=`/`LON=`) so a reboot
  keeps it. A **"Use my location"** button fills it from the browser's GPS, with a
  graceful fallback (browsers only allow geolocation on a secure/localhost page).
- **Device card** (Settings) — firmware / hardware / Astro-Kit axis / clock, from
  `780 SP_GET_DEVICE_VERSION` + `781 SP_GET_SYSTEM_TIME` (added to the allowlist).
- **The observing position is no longer required to start.** `polaris-httpd` and
  the boot script used to refuse to run without `LAT`/`LON` in `site.conf`; now the
  server comes up regardless, so the page is reachable and you set your location in
  the web app (the browser's GPS or by hand) — which persists it back to `site.conf`
  for next boot. No `site.conf` editing, no SSH. The Astro tab shows a **"set your
  observing location"** prompt until it is set, and alignment/go-to are gated on it,
  while framing, focus, the gallery and the programmes work without it.
- **Zero-SSH astro install — bake the stack into the firmware.**
  `patch-polaris.sh --astro-autostart` copies the pre-built astro binaries and
  scripts straight into `/app/astro` inside the patched firmware and installs
  `polaris-astro-boot.sh` as the boot hook. So the SD card carries **only the
  index files** — flash the firmware, drop `astrometry/` on the card, power on,
  open the page, set your location. No SD-side binaries, no `install_astro.sh`,
  no SSH. A `--ssh-key` hook is preserved as the boot script's chained pre-hook.
  Updating means re-flashing. The bake's copy (indexes and `site.conf` correctly
  left OUT), boot-hook install and SSH chaining are verified against a fake appfs;
  the full patch needs a stock firmware image, so it and the on-hardware boot have
  not been exercised. (The boot script now detects the SD by its index dir, not by
  `site.conf`, so first boot doesn't stall waiting for a config that isn't there.)

### Fixed

- **A selectless exposure slot halted the boot script.** The new AF/MF control is
  an `.exposlot` with no `<select>`, and the exposure loader swept every
  `.exposlot` doing `select.addEventListener` — throwing on it and stopping the
  boot before the later wiring ran. The three exposure sweeps are now scoped to
  `.exposlot[data-get]`.
- **Autofocus would not have worked on the device: `autofocus.sh` was never
  bundled.** `polaris-httpd` runs `$ASTRO/autofocus.sh` (`ASTRO` defaults to
  `/app/astro`), but the script lived in `container/astro/` while the build only
  copies `container/astro/ondisk/*.sh` into the image — so it shipped `solve-now.sh`
  and `polaris-guide.sh` but not `autofocus.sh`. Moved it into `ondisk/` beside its
  runtime peers, so `build-astro.sh` bundles it and `install_astro.sh` installs it.
- **Removed five accidental duplicate docs** (`… 2.md`, Finder/iCloud copies — four
  byte-identical, `CAPTURE-PATH 2.md` a stale 352-line copy of the 549-line
  canonical), and added a complete **HTTP API reference** to `docs/ASTRO.md`
  covering every `/api/*` route.
- **Runtime site save persisted to the wrong file.** `persist_site_latlon` wrote
  `$ASTRO/site.conf` (`/app/astro/site.conf`), but the boot script reads
  `/app/sd/polaris-astro/site.conf` — so a saved location updated the running
  server yet would not have survived a reboot, contradicting the UI. It now writes
  the boot-read file (new `--site-conf`, defaulting there), preserving the other keys.
- **A misleading-indentation warning in the device build.** `indi_one` had
  `if (cap) out[0]=0; return 0;` on one line — harmless, but gcc's
  `-Wmisleading-indentation` (which the ARM cross-build sees but host clang did
  not) flagged it. Split onto two lines, and the cross-build now compiles the
  first-party tools with **`-Werror`**, so a warning fails the build the way the
  host tests already do. Verified: `build-astro.sh` produces the full SD bundle
  clean, with `autofocus.sh` and all new endpoints in it.

### Fixed — real-sky session, 2026-08-19

- **Plate solving failed on every real frame because the focal length was
  wrong.** `site.conf` said `FOCAL=400` while the lens was a 24–70 zoom at
  70 mm. The pixel-scale search is derived from the focal, so the solver hunted
  2.3″/px when the truth was 13.4″/px and no quad could match — at any pointing,
  under any sky. The same frames solve in ~5 s at the right focal (log-odds 132
  and 243 against a threshold of 100). It looked like a direction-dependent sky
  problem and was neither.
- **The solver now works the focal out for itself**: manual override (new web UI
  control) → the frame's EXIF → the focal cached from the last EXIF frame →
  `FOCAL` → a search across `FOCAL_MIN`–`FOCAL_MAX` (8–3000 mm). EXIF beats the
  config because a config value cannot follow a zoom ring. `polaris-extract`
  parses EXIF `FocalLength` from APP1 while it is already decoding the JPEG.
- **The autosolve capture fallback wedged the camera** and is removed. On a
  failed live-view solve it fired the `272` lapse sequence; astro mode refuses
  that but still leaves a lapse task established that nothing aborts, producing
  "shot failed" and a camera that will not capture again until it is
  power-cycled *and* the USB replugged. The daemon now issues no capture
  commands at all. The reason this cannot work was already recorded in
  `solve-now.sh` and `polaris-autoalign.sh` before it shipped here.
- **Alpaca `Altitude`/`Azimuth` read the mount with no alignment gate.**
  Position is the first thing every Alpaca client polls on connect, and
  discovery advertises this box on UDP 32227, so a running ConformU/NINA/
  Stellarium would find it and open a connection to the control port per poll
  while unaligned — which is what makes the Benro app demand a compass
  calibration, with no action from the user. Now gated like the rest.
- **`RightAscension`/`Declination` answered 0/0 when they knew nothing**, which
  draws the telescope at the vernal equinox in any planetarium app. They now
  report the position as unknown.
- **Failed solves keep the frame** under `/app/sd/polaris-astro/failed/` with the
  extracted star count, so a repeatable failure can be diagnosed instead of
  guessed at.
- The web server starts at boot again (`HTTPD_WAIT_ALIGNED=0`); the protection
  is in the endpoints, which refuse every mount read until the mount is aligned.
  Measured: zero connections to the control port from a full sweep of the page,
  all four Alpaca position properties, and the focal endpoint.

### Added
- **`./build-astro.sh`** — one command that cross-builds the plate solver for the
  device, downloads the index files a given focal range needs, and assembles
  `out/astro-bundle/` ready to copy to the Polaris. Purely additive on the
  device (`/app/astro`); nothing is flashed and `rm -rf /app/astro` undoes it.
- **`container/astro/`** — `build_solver.sh` (ARM cross build with ABI, glibc,
  DT_NEEDED **and licence** guards), `polaris-solve.c` (MIT front end to
  astrometry.net), `polaris-extract.c` (MIT JPEG→star-list extractor),
  `gslshim/` (BSD-3 GSL replacement, differential-tested against real libgsl),
  `fetch-indexes.sh`, `bench-solve.sh`, and the device-side `ondisk/` scripts.
- Measured on an 8192×6144 frame at 1.9789″/px — exactly 400 mm on full frame —
  the pipeline solves to within ~22″ of the reference in 0.37 s of extraction
  plus 2.67 s of solving, under double emulation. See
  [docs/BENCH-RESULTS.md](docs/BENCH-RESULTS.md).

## Superseded notes — design phase

### Added
- **`docs/LICENSE-AUDIT.md`** — component-by-component licence audit of the
  astrometry.net 0.98 tree. Key finding: replacing the GPLv3 `gsl-an` is **not**
  sufficient — `qfits-an` (including `anqfits.c`), which the solver needs to read
  index files, is **GPL v2-or-later**, and `util/ctmf.c` (GPLv3, via `simplexy`)
  and `catalogs/brightstars` (GPLv2+, Stellarium-derived) are further traps.
  Resolution: the solver ships as its **own process** under GPL v2+, nothing GPL
  is ever linked into `pgphoto`, `polestar_app`, or our MIT loader, and the
  in-app hook marshals to it rather than linking it. Includes an advisory that
  Aperion currently links `libqfits.a` + `libcatalogs.a` into an App Store binary.
- **`docs/PLATE-SOLVING.md`** — feasibility study + architecture for on-device
  astrometry.net plate solving and automatic alignment: verified device facts
  (dual-core ARM, 1536 MB RAM, NEON/VFPv4, ~20.4 MB appfs headroom), the reusable
  assets already in the firmware (`polestar_app` is unstripped with DWARF and
  statically links OpenCV + a `starskystacker` star extractor + `SP_OneStarCal`;
  lighttpd and busybox httpd are already on the device), the BSD-3 licensing model
  copied from Aperion's GSL shim, a three-layer architecture, a multi-frame
  pointing-model solve, and a phased plan that starts with measurement.
- Recorded as a **prerequisite bug fix**: the R5 II capture shim currently forces
  `capturetarget = Internal RAM`, so nothing is written to the camera's own card.
  Capture must write to the camera card *and* download to the Polaris SD.

## Unreleased — optional SSH debug access + Windows build fixes

### Added
- **`--ssh-key` / `-SshKey` (opt-in): authorise a public key for root SSH login.**
  The stock firmware already runs OpenSSH 7.8p1 (`/usr/local/bin/sshd`, started
  by `/etc/init.d/rcS`, `PermitRootLogin yes`); only a key in `/root/.ssh` was
  missing. The patcher **modifies no firmware file** — it adds the optional boot
  hook `/app/bootapp` already calls if present (`network_telnetd.sh`, falling
  back to `start_agent.sh`), which appends your key(s) to
  `/root/.ssh/authorized_keys` at boot (idempotent; existing keys kept) and fixes
  the `StrictModes` permissions. Aborts instead of editing `bootapp` if the
  firmware calls no hook it can claim. Accepts a `.pub`/`authorized_keys` path or
  a literal key line; repeatable.
- **`container/gen_ssh_hook.py`** — strict key validation (type allow-list, base64,
  SSH wire-format body matching the type; private keys refused) plus `SHA256:`
  fingerprints (identical to `ssh-keygen -lf`) in the log and in the hook's header.
- **`out/ssh-debug/`** — the exact hook that went into the image, standalone, with
  instructions to install it **without flashing** and to remove it.

### Fixed — Windows (issue #1)
- **CRLF checkouts broke the container** (`/bin/bash^M: bad interpreter`, which
  surfaced as `exec /opt/patcher/patch.sh: no such file or directory`).
  Added **`.gitattributes`** pinning LF for everything the container consumes
  (CRLF only for `*.ps1`), and the **Dockerfile now strips stray `CR`s after
  `COPY`** and asserts `patch.sh`'s shebang, so clones made before the fix work
  too. This also protects reproducibility: normalising a CRLF checkout restores
  `pgphoto.wrapper` to its hardware-validated md5 `868c3097…`.
- **`patch-polaris.ps1` no longer reports success after a failed build/run.**
  `docker build` is no longer quiet-with-output-discarded, and both `docker build`
  and `docker run` exit codes are checked (the old script printed `[OK]` and the
  output paths even when the container never ran).
- **`patch-polaris.ps1` bind-mount paths** are normalised for Docker Desktop
  (provider path → drive letter + forward slashes, trailing separator trimmed,
  clear error on UNC paths instead of a cryptic mount failure).

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
