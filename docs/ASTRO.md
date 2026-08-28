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

# FOCAL IS ONLY A FALLBACK -- the solver reads it from each frame's EXIF, which
# is the only thing that follows a zoom lens. See "Focal length" below.
FOCAL=70
FOCAL_MIN=8            # range searched when no focal is known at all
FOCAL_MAX=3000
RANGE_TIMEOUT=240      # that search needs ~148 s; the normal path is ~5 s

# Web UI + Alpaca on :8090.  (8080 is pgphoto's own MJPG stream; 80, 8081,
# 9090 and 22 are also taken.)
POLARIS_HTTPD_PORT=8090

# The web server starts at boot; it refuses to READ the mount until aligned.
# Set 1 to not start it at all until an alignment has completed.
HTTPD_WAIT_ALIGNED=0

# Auto-solve during the app's calibration.
AUTOSOLVE=1
AUTOSOLVE_DRY_RUN=1      # 1 = log what it WOULD do, touch nothing. Start here.
MIN_LOGODDS=100          # solve-quality gates; below these it does nothing
MIN_MATCHES=12
CENTRE_TOL_DEG=0.15      # how well centred before it will confirm (~9 arcmin)
MAX_ALIGN_ALT=85         # hard stop near the zenith (see below)
SOLVE_ARCSEC=60          # assumed solve accuracy, for the uncertainty check
MAX_UNCERT_FRAC=0.25     # refuse if uncertainty exceeds this fraction of the correction

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

## How the focal length is resolved

A wrong focal length is the single most effective way to make plate solving fail
completely, so this is worth understanding. The pixel-scale search is derived
from it: search 400 mm on a 70 mm frame and the solver looks for 2.3″/px when
the truth is 13.4″/px, so **no quad can match, at any pointing, under any sky**.
This is not hypothetical — an entire night of frames failed exactly this way,
with `FOCAL=400` in the config and a 24–70 zoom sitting at 70 mm. The same
frames solved in ~5 s once the focal was right.

In order, each falling through to the next:

| # | source | when it applies | cost |
|---|---|---|---|
| 1 | manual override (web UI) | you set one; for scopes/manual lenses | ~5 s |
| 2 | the frame's own **EXIF** | any real capture | ~5 s |
| 3 | the focal cached from the last EXIF frame | live view, which carries no EXIF | ~8 s |
| 4 | `FOCAL` in `site.conf` | nothing above is available | ~5 s |
| 5 | search `FOCAL_MIN`–`FOCAL_MAX` | nothing is known, **or** the focal tried above failed | **~148 s** |

EXIF beats the config file deliberately: a config value cannot follow a zoom
ring, and the config is what was wrong. The manual override beats EXIF because a
telescope or adapted lens has no electrical contacts and reports nothing, and a
body will stamp a stale value in for one — a number you typed is better
information than anything we can infer.

Measured on the device, same frame each time: EXIF 8 s, cached focal 8 s, full
range search 148 s. The range search is the safety net, not the normal path, so
it gets its own `RANGE_TIMEOUT` (240 s) — with the caller's 45 s budget it
always timed out and never actually caught anything.

**Index coverage limits this.** The shipped indexes (4110–4119) cover roughly
14–400 mm on full frame. Widening `FOCAL_MAX` does not by itself make a 3000 mm
lens solvable; that needs finer indexes — see `fetch-indexes.sh`.

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

## The control app

`polaris-httpd` now serves a full control surface at `/` — live view, pan/tilt/
rotation jog, the camera's own exposure lists, shutter and record, battery and
card status, and the Wi-Fi settings. It is a recreation of what the Benro
Connect phone app does, running on the head and served to any browser, so the
phone stops being required equipment. The protocol behind it is documented in
[`APP-PROTOCOL.md`](APP-PROTOCOL.md) (122 opcodes) and the app's own feature set
in [`APP-FEATURES.md`](APP-FEATURES.md).

Three things about it are worth knowing before using it in the field.

**One connection, held open.** Everything reaches the head through a single
registered TCP session (`polaris-link`), the way the phone app does, instead of
running `polaris-mount` per request. That is what makes a joystick possible —
telemetry arrives at ~30 Hz instead of a five-second cache — and it doubles as
the radio keepalive, so `wifi-keepalive.sh` is redundant while the server runs.
It refuses to connect until the mount is aligned, for the reason that script
documents: registering while unaligned makes the Benro app demand a compass
calibration. `POLARIS_LINK_FORCE=1` overrides it for bench work.

**The stop button is on the server, not in your browser.** Fast jog has to be
re-sent every 50 ms and *nobody has established that the head stops on its own
when the stream stops*; slow jog latches until it is explicitly released. So the
browser never drives the wire — it declares an intent with a 400 ms lease and
the server owns the repeat and the stop. A phone that locks, a tab that closes
or a Wi-Fi drop mid-slew therefore **stops** the head rather than abandoning it
in motion. `Esc`, the STOP button, leaving the Control tab, or backgrounding the
page all stop everything immediately.

**Nothing is guessed.** Only opcodes with a payload documented in
`APP-PROTOCOL.md` are reachable, by name, default-deny; anything that moves a
motor or fires the shutter additionally needs an explicit confirm. `530`
(multi-step alignment, which wedges the motors) and `542` (which *releases* the
travel limits — note the inverted polarity) are deliberately not exposed.

**Timelapse and panorama are wired** (the Programs tab). Both run on the head,
and because the server owns the progress poll they keep running — and keep being
watchable here — with no browser connected, which the phone app cannot do. Each
start moves the mount and fires the shutter, so it is a two-tap arm-then-confirm.

Panorama carries one honest caveat: its start payload is INFERRED. The field
order and units are read straight from the app, but the method that binds the
stitch/limit flags would not decompile (docs/APP-FEATURES.md, panorama pitfall
14), so the UI shows the exact frame it will send and says the binding is a
guess until it is confirmed on hardware — watch the first run. Timelapse has no
such caveat; its payload is read directly from the app's layout classes.

**HDR (280) and focus stacking (270) are wired too.** Both payloads are read
directly from the app's own `getStartShootingParameter()` builders, so unlike
panorama neither is inference:

- **HDR** is a three-frame shutter bracket around the camera's current exposure.
  Because the head speaks exposure INDICES, the bracket walks the shutter a
  chosen number of steps either side of the current index and holds the other
  axes — and the preview resolves those indices back to the real shutter speeds
  from the camera's own list, so you see "1/250 / 1/60 / 1/15" before it fires,
  never a raw index. It refuses to run until the camera's shutter list has
  loaded (open the Control tab once), because bracketing without it would be
  guessing. The head PUSHES HDR progress (`step:5` remaining, `step:3` done)
  rather than being polled.
- **Focus stacking** racks the lens between two marked limits and stitches an
  all-in-focus frame. Set manual focus, rack with the four hold-repeat buttons
  (they go through the ordinary 311 focus-adjust), mark near and far, then start
  or dry-preview. Its only number is the frame count (2–200). The start frame is
  `step:3;;num:<shots>;` — the double semicolon is the app's own, not a typo.

### ASTRO mode and alignment

The Astro tab drives celestial mode from the browser. Entering it sends
`SP_SET_MODE_STATE(8)` and holds the AHRS attitude stream alive with a
server-side heartbeat (re-armed every few seconds — the app re-arms it every 5,
and a browser tab cannot be trusted to); leaving stops tracking and returns to
photo mode. Tracking is `SP_SET_TRACK_AU_STATE` (531) with a sidereal/lunar rate
and a full/half toggle — and the half-rate flag is inverted on the wire
(`halfSpeed:1` = half), which the module handles.

**Tilt compensation — track on an unlevel tripod.** The Astro tab surfaces
`SP_SET_TILT_STATE` (538), a head-side setting the phone app buries three taps
deep in a help sheet: *"When Polaris is not horizontal, turn on tilt compensation
to compensate for the tilt angle. If it is off, be sure to level the device."*
With it on, the head corrects for its own tilt from its attitude sensor, so
deep-sky tracking does not need a perfectly level tripod — only a base that isn't
moving. It is persistent and it measurably changes tracking accuracy, so the
toggle reads the head's current value (537) when you enter astro rather than
defaulting. Beside it, **auto-level** (549) physically drives the head to level;
it *moves*, so it is confirm-gated and must be done **before** aligning, never
while tracking. With tilt compensation on you can usually skip it. These are the
two different things the app is careful to separate (`docs/APP-FEATURES.md 4.4`):
a setting that tolerates tilt, versus an action that removes it.

**Compass alignment without a phone.** The head has to know which way is north
before it can point at a star; the app sends its handset magnetometer heading
(`SP_SET_YAW`, 527). The browser offers three routes, best first:

1. **Solve the sky** — take a frame and let this repository's own solver work
   out exactly where the head is pointing, then write that as the alignment
   (`/api/solve?apply=1`). No compass, no phone sensors, and strictly more
   accurate than a magnetometer. This is the path to prefer.
2. **Phone compass** — when the browser is on a phone held against the head, read
   its `DeviceOrientation`/`webkitCompassHeading` and send that as 527 (iOS asks
   permission on the first tap).
3. **Type a bearing** — a heading read off a real compass, as a last resort.

All three end in the same 527 the app sends, with the server's own lat/lon so
alignment and the solver agree about where the head is. 527's `ret` polarity is
documented as contradictory, so alignment is confirmed by the mount's reported
state, never by 527's reply.

### FREE PROGRAM — a keyframe move you fly by hand

The Programs tab's **Program** mode is the app's FREE PROGRAM (283): fly the head
with the jog, capture its live pose as a keyframe, fly to the next, capture
again, then play. The head walks the whole path on its own and — being
head-side — keeps going with no browser attached. Poses are captured from the
head's own 517 attitude (radians) so the stored keyframe needs no coordinate
maths. Motion interpolates linearly between keyframes or holds (step) at each;
an optional photo track shoots along the way. The exact timeline is previewed
before it is uploaded.

### PATH-LAPSE and SUN

**PATH-LAPSE** is a moving timelapse, and it is not a new command — it rides the
**same 272 opcode** as the static timelapse. You fly the head and capture 2–8
waypoints; each one after the first carries the number of frames to shoot on the
leg that reaches it (or ∞ for the whole leg). On the wire each waypoint is a
`step:2` point with the gimbal pose in radians and a **cumulative** arrival time
(the sum of the earlier legs' `frames × interval`), and `SEND_END` reports the
summed `photoCnt`. Because it is 272, it shares the timelapse's remaining-count
poll, its run view and its dead-man exactly — nothing new to trust. The frame
counts come from the app's own `DynamicLapseLayout` builder, so the payload is
read, not inferred; the timeline is previewed before the two-tap confirm. The
poses are captured server-side (like FREE PROGRAM), so what you fly to is what
is stored.

**SUN** schedules a sunrise/sunset timelapse across a time window (277): the head
does the solar GOTO itself and parks when the window ends, pushing its own
progress the way HDR does rather than being polled. Two things are load-bearing.
First, the window has to be within an hour of *now* and at least three minutes
long — the head enforces both, so the browser checks them up front with a clear
message. Second, the check and the serialised times use the **browser's** clock,
not the device's: the Polaris runs local time while reporting itself as UTC (the
same clock trap the plate solver already corrects for), so trusting it here would
schedule the shoot hours off. The window is sent as the app's
`yyyy,MM,dd,HH,mm,ss` local wall-clock, `sun:0` for sunrise and `sun:1` for
sunset.

### HOLY GRAIL — the day-to-night exposure ramp

The last programme, and the odd one out: **it is not a run, it is a
configuration.** It uploads a target-brightness curve — and, optionally, the
three exposure-axis ranges — that the head then follows *while a timelapse or
path-lapse runs*, ramping exposure smoothly from daylight into night. So the
Holy Grail tab configures the ramp; you start the actual capture from Timelapse
or Path-lapse.

Two caveats are load-bearing, and the UI states both:

1. **The ramp meters through an external accessory.** The head reads ambient
   light from the *Optical Matrix Sensor Module*, not from the images; without
   that accessory it cannot ramp. A web app can upload the curve the head will
   follow, but it cannot replace the sensor.
2. **The axis-value and priority encodings are inferred.** The 305 step→field
   shapes are read from the decompile (`SET_GRAIL_MODEL`, `SET_PRIORITY`,
   `SET_ISO`, `SET_F` as `%.1f`, `SET_SHUTTER`, and the `nodeCnt;para:<Δmin>/<ev>`
   curve), but whether the shutter handles are indices and what integers name the
   priority axes are best-guesses. So — exactly like panorama — the whole 305
   batch is previewed before it is sent, and the exposure limits are picked from
   the camera's *own* lists (ISO with the app's Auto-filter and 6400 cap) so
   nothing is invented.

The curve's offset-0 anchor is mandatory, offsets snap to 30-minute lines and EV
to 0.5 over a 24 h span, and the runtime-brightness readback ignores the SET acks
that share the 305 reply slot so it never shows a stale value as a live reading.

That is every programme the Benro Connect app exposes now driven from the browser
— timelapse, path-lapse, panorama, HDR, focus stack, sun, astro, the FREE
PROGRAM timeline and the Holy Grail ramp.

### Live view

The stream is the head's own mjpg-streamer on **:8080**, referenced directly
rather than proxied — proxying it through this single-threaded server would
block every other request for as long as the stream is open.

## Web UI

`http://<polaris ip>:8090/` — solve status, last solution (RA/Dec, roll, pixel
scale, field size, match count, solve time), a live log tail, and buttons to
solve the latest frame or wait for the next one.

**The page never polls the mount.** Every connection to the control port is seen
by the device as an app connecting, and polling made the Benro app re-prompt for
compass calibration continuously. Mount state is read passively from the
device's own log; an active read happens only when you press **Read Mount**, and
only when the mount is already aligned.

### Focal length

The **Focal length** card shows what the solver will use and where it came from,
and lets you override it. Auto is the normal mode; type a number and press
**Set** for a telescope or a manual/adapted lens, which report no focal length
at all. **Auto** returns to detection.

A wrong focal does not make solving slow, it makes it **impossible** — the
pixel-scale search is derived from it, so a 70 mm frame searched at 400 mm hunts
2.3″/px when the truth is 13.4″/px and no quad can ever match. Values outside
1–100000 mm are rejected rather than stored.

### The server starts at boot, and reads nothing until you align

`HTTPD_WAIT_ALIGNED=0` is the default: the page is there when you open it. The
protection is in the endpoints, not in the server's absence — **every** path
that would open a connection to the control port refuses while the mount is
unaligned, because connecting to an unaligned mount is what makes the Benro app
demand a compass calibration.

That gate covers `/api/state?mount=1` (reports `mount_blocked`), the LX200
position replies, and the Alpaca `Altitude`, `Azimuth`, `RightAscension` and
`Declination` properties. Everything else on the page — alignment state, the
last solution, the log — comes from the device's own log and files, which cost
nothing.

Two things worth knowing:

- **Alpaca clients find this box by themselves.** Discovery advertises it on UDP
  32227, so a running ConformU, NINA or Stellarium will connect and start
  polling position with no action from you. That is fine now, but it is the
  first thing to suspect if a compass prompt ever appears unexplained. Start
  with `--no-discovery` to disable it.
- **The gate only covers the unaligned window.** Once aligned, a polling client
  opens a connection per position read, as it must.

Set `HTTPD_WAIT_ALIGNED=1` to go back to not starting the server at all until an
alignment has completed.

To reach the device from your ordinary network instead of its access point, see
[NETWORKING.md](NETWORKING.md) — it can join your home wifi on a second
interface while the access point keeps serving the Benro app.

---

## How high is too high to derive a heading?

Azimuth error is amplified by 1/cos(altitude), which makes it tempting to refuse
well short of the zenith. The first cut refused above **65°** — and that was an
instinct, not a calculation. It threw away a perfectly good solve at 70.4° on the
first real night out.

What matters is the amplified error *compared with the compass error being
corrected*:

| altitude | 1/cos | a 40″ solve becomes |
|---|---|---|
| 65° | 2.4× | 95″ = 0.03° |
| 75° | 3.9× | 154″ = 0.04° |
| 80° | 5.8× | 230″ = 0.06° |
| 85° | 11.5× | 460″ = 0.13° |

The compass error being corrected is **tens of degrees**. Even at 85° the
amplified uncertainty is about a tenth of a degree — still an enormous
improvement on not correcting at all.

So the real test is not the altitude but whether the correction is worth making:
the daemon computes the amplified azimuth uncertainty and refuses only if it
exceeds `MAX_UNCERT_FRAC` (0.25) of the correction itself. On last night's
refused solve — alt 70.4°, correction 44.6°, uncertainty 0.05° — it now proceeds.
The gate only bites on marginal corrections: at 80° the correction must exceed
0.38° to be accepted.

`MAX_ALIGN_ALT` remains as a hard stop, now at **85°**, because that close to the
zenith an alt-az mount's azimuth axis is ill-conditioned mechanically as well as
mathematically.

---

## The device locks up under TCP load (a stock defect, worked around)

Symptom: every TCP service stops answering — ssh accepts the connection and
never sends a banner, the web page and the Benro control port go silent — while
**ping keeps replying** and the access point stays up. Shortly after, the device
reboots itself.

Cause: the wifi driver floods the kernel log with

```
dhd_tcpdata_info_get 1056: No more free tdata_psh_info!!
```

whenever its TCP-flow pool is exhausted, which on this firmware is routine (414
of 943 lines in the kernel buffer were this one message). The kernel command
line is `console=ttyAMA0,115200`, so **every one of those lines is written out a
115200-baud serial port** — roughly 5 ms of blocked kernel time each. A few per
second is invisible; hundreds per second is a kernel that does nothing but
print. ICMP still answers because it is handled in softirq; every TCP listener
starves.

The fix is one line, applied at boot before anything else starts:

```sh
echo 1 > /proc/sys/kernel/printk
```

The messages still land in the kernel ring buffer, so `dmesg` keeps them for
diagnosis; they simply no longer block the system to print. `PRINTK_QUIET=0` in
`site.conf` opts out.

Nothing in this project generates those messages — we only add TCP traffic that
makes the pool churn faster. This is very likely a **stock defect** and may
explain flakiness other owners see.

---

## The Wi-Fi turns itself off 60 seconds after the app disconnects

This is stock firmware behaviour, not a fault in this project, and it surprises
everyone who tries to use the Polaris headlessly. Captured from the device's own
log at the moment it happens:

```
01:41:16  SP_ClientCtxDel: id[3], type[wifi]; WifiCount[0]   the app disconnected
01:42:16  WifiBtTask[201]: wifi auto off                      exactly 60 s later
01:42:16  SP_SetWifiState[0]
01:42:17  remove@/bus/platform/drivers/bcmdhd_wlan            driver unloaded
01:42:17  MsgFromWifiBt --> val[wifi:0; bt:1;]                wifi off, bluetooth on
```

Sixty seconds after the **last** Wi-Fi client disconnects, the firmware powers
the radio down and unloads the driver, leaving Bluetooth as the wake path. The
SSID disappears; ssh, this web page, Alpaca and LX200 all go with it. There is
no setting for it — `/app/wifi/` holds only start/stop scripts, and the timer
lives inside the `polaris_wifi_bt` binary.

### Keep Awake — opcode 808 is what registers a client

Simply holding a connection open does **not** work, and understanding why took a
protocol capture. A plain TCP connection is never entered into the client table:
it raises `SP_EVENT_APP_CONNECT` and is even given an id, but no
`SP_ClientCtxAdd` follows, and closing it gives `SP_ClientCtxDel: not find this
is id[N]`. Sending ordinary protocol messages on it changes nothing.

Capturing a **real app connecting, from the first byte**, showed what does:

```
rcv msg from App[5]: type:2; code:808; val:type:0;
SP_MsgSysFromAppProc: client type[0]; id[5];
SP_ClientCtxAdd: id[5]; type[wifi]; WifiCount[1], CellCount[0];
SP_SendMsgToApp: code[808], val[ret:0;]
```

The app announces itself with **opcode 808, `type:0`** after its initial
property sweep. The device maps client type 0 to `type[wifi]`, increments
`WifiCount`, and acks `ret:0`. Sending exactly that from our own connection
produces the same three lines and takes `WifiCount` from 1 to 2 — and closing it
now gives a *found* `SP_ClientCtxDel`, not "not find".

So **Keep Awake** connects, sends `1&808&2&type:0;#`, and holds the socket. It
re-registers on reconnect. Stored on the microSD, restored at boot, and **off by
default**:

- **It costs battery** — the radio stays powered.
- **It waits for alignment before connecting.** Registering while the mount is
  unaligned is what makes the Benro app demand a compass calibration, so until
  the mount is aligned the page shows it as *armed* rather than active.

**How alignment is known, and why it used to be missed.** It cannot be
rediscovered by grepping `/app/Mlog.txt`: the device truncates that file, and it
has been observed holding *no* `284` lines minutes after an alignment, with the
mount reporting `{"mode":8,"track":1,"aligned":true}`. Every component that
re-derived alignment from the log therefore concluded "not aligned" for a mount
that was aligned and tracking, and the keepalive sat parked. So the state is
written down when it is legitimately learned: `polaris-mount state` records the
track value to `/tmp/polaris-track`, and the web server both records and reloads
it. Anything that needs to know reads the file, without opening a connection of
its own. It is in `/tmp` deliberately — alignment does not survive a power cycle
either.

**What is and is not proven.** With Keep Awake on, the Wi-Fi survived closing the
app — where before it went down 60 s after the last client left. One caveat
worth knowing: a phone that closes the app often leaves a **stale TCP
connection** behind (observed at 27 KB stuck unsent, surviving 4+ minutes), and
while that lingers it is still a counted client. So the moment that finally
proves Keep Awake alone is enough is when that stale socket expires and
`WifiCount` falls to 1 rather than 0. Our client is registered, so it should —
and `wifi-watch.log` records the answer either way, including a `wifi auto off`
if one ever fires.

> **The capture log contains credentials.** Opcode 790 returns the device's Wi-Fi
> password and security answer (base64). `/app/sd/app-connect-capture.log` and
> anything derived from it should be treated as sensitive — delete it when done,
> and do not attach it to a bug report unedited.

---

## Guiding from the web page

The **Guiding** card starts and stops the guider and plots what it measures.
The chart is drift in arcseconds against time: a blue line per check, yellow
dots where drift exceeded the threshold and a correction was made, and a dashed
red line at the threshold itself. Below it: how many checks, how many
corrections, and the peak drift.

The points come from parsing the guider's own log rather than from a second
tally kept alongside it, so the chart cannot disagree with what the guider
actually did.

It refuses to start in two cases, both deliberate:

- **Nothing has been solved yet** — there is no anchor to guide on, so it would
  start and immediately give up.
- **The mount is not aligned** — guiding issues slews, and slewing against an
  unknown pointing is worse than drifting.

Corrections are small slews rather than rate adjustments, because the mount
exposes no axis-rate primitive. `GUIDE=1` in `site.conf` additionally lets
autosolve start the guider by itself after a successful alignment; left at 0,
guiding only happens when you press the button.

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

### Conformance

Validated with **ASCOM ConformU 4.5.0**, the official checker, not just our own
suite:

| suite | result |
|---|---|
| Alpaca protocol | **0 issues**, 1 error, 21 advisory |
| Telescope device | 23 issues, **0 errors** |

The protocol suite's single error is a contradiction between ConformU's own two
suites: the DEVICE check requires `IsPulseGuiding` to raise NotImplemented when
`CanPulseGuide` is False, while the PROTOCOL check calls it during polling and
treats that as fatal. Satisfying one breaks the other; we follow the device
check, which reflects the spec.

The device suite's remaining 23 are the mount, not the driver: ~10 slew results
in the 10-55 arcsec range against a 10 arcsec tolerance (the mount points to
6-9"), and ~6 from repeated syncs, which this mount ignores by design
("send it ONCE").

Our own quick suite is still there for a fast check:

```sh
python3 tests/alpaca_conformance.py http://<polaris ip>:8090
```

Note it is *our reading* of the spec -- it reported 35/35 while ConformU found
60 real issues, including a 7-hour SiderealTime error. Trust ConformU.

## INDI (native)

KStars/Ekos and PHD2 are INDI-native, and LX200 cannot express what they need
for real imaging — track state, sync as distinct from slew, an async slew you
can abort. So the server also speaks **native INDI on TCP 7624** (add an *INDI
Telescope* device pointing at `<polaris ip>:7624`). It is a hand-rolled minimal
`INDI::Telescope`, the same way the Alpaca device is hand-rolled — deliberately
**no libindi**, whose C++/cfitsio/libnova stack will not cross-compile on the
debian:9 ARM toolchain. Like the LX200 server, each client is a forked child and
the pointing is pushed ~1 Hz.

Properties implemented, all mapped to the same `polaris-mount` commands the
Alpaca/LX200 paths use:

```
CONNECTION                 connect (network driver, always up)
DRIVER_INFO                DRIVER_INTERFACE = 1 (telescope)
ON_COORD_SET               TRACK | SLEW | SYNC
EQUATORIAL_EOD_COORD       RA/DEC -> goto-radec (slew, async Busy) or align (sync)
TELESCOPE_ABORT_MOTION     -> polaris-mount abort (works during a slew)
TELESCOPE_TRACK_STATE      TRACK_ON / TRACK_OFF
GEOGRAPHIC_COORD           LAT / LONG (0-360 E) / ELEV
TIME_UTC
```

The slew runs in the background so ABORT stays responsive, and the vector goes
`Busy` → `Ok` when the pushed pointing lands within ~0.5° of the target (or after
60 s). Position reads reuse `current_radec`, which is connection-safe at 1 Hz and
never connects to an unaligned mount.

---

## What is and is not verified

### Verified on hardware

| | |
|---|---|
| Capture to camera card **and** Polaris | JPEG ~9.4 MB, ~0.94 s announce |
| Solver at live-view resolution (960×640) | 20–40″ over four fields, 1–4 s |
| Real R5 II frame solved on device | 6.4″ vs upstream, 9.3 s |
| **Real night sky solved on device** | 2 frames, log-odds 132 / 243, 27 / 48 matches, ~5 s |
| Keep Awake registers as a client | `SP_ClientCtxAdd id[11] type[wifi] WifiCount[2]` |
| Concurrent AP + station (same channel) | joined in 1 s, AP unaffected, survives reboot |
| Auto-join at power-up | cold boot → joined in 12 s, first attempt |
| Console-flood lockup fix | 80 TCP connections driven through, stayed responsive |
| Wi-Fi survived closing the Benro app | with Keep Awake on; previously died 60 s later |
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

### Verified with the REAL app, simulated sky

A genuine calibration — real `519` goto, real `530 step:1` — against a sky
rendered at the app's own target offset by a known amount:

| | target az | rendered az | solved az | recovered |
|---|---|---|---|---|
| clock uncorrected | 224.781 | 229.781 | 229.787 | 5.006° |
| clock corrected | 183.331 | 188.331 | 188.343 | 5.012° |

Both recover an injected 5.000° to within 43 arcsec, well inside the 0.15°
centring tolerance. The device's absolute time was separately cross-checked
against an independent implementation: the residual is 8–23 arcmin, which is
precession from J2000 to the current epoch (26.6 yr x 50.3"/yr ~ 22 arcmin), not
error. A clock fault would show as ~105° of RA, not fractions of a degree.

### NOT verified

- **Real stars in a LIVE-VIEW frame.** Full-resolution night frames from a
  light-polluted backyard (background 111/255) now solve on the device in ~5 s,
  so the sky is no longer the open question it was. But every real-sky solve so
  far has been a full capture; whether a 960×640 live-view frame carries enough
  signal under *your* sky is still unanswered, and it is the frame the
  calibration path actually uses.
- **A confirmed alignment on real sky.** The armed path has been driven for real
  in dry run; nothing has yet written a heading correction to a live mount
  outside the simulator.
- **An end-to-end calibration since the focal and capture fixes.** Those were
  verified against saved frames and per-endpoint, not by running the app through
  a real alignment. That run has not happened yet.
- **Any camera other than the R5 Mark II**, and any firmware other than 4.0.0.32.
- **The camera cold-start delay is NOT fixed.** After a cold start
  `gp_camera_init` times out on PTP for minutes and shots fail until it clears.
  Diagnosed, not solved — see [CAPTURE-PATH.md](CAPTURE-PATH.md). Three
  plausible-looking fixes were tried and disproved by the logs.
- **Concurrent AP+station across two BANDS crashes the firmware.** Same channel
  works; different bands takes the whole device down. See
  [NETWORKING.md](NETWORKING.md).

### Failed solves keep their evidence

A solve that fails writes the frame to `/app/sd/polaris-astro/failed/` along with
the star count extracted at the same downsample and cap the solve used, bounded
to `KEEP_FAILED` (20) frames. That count is what separates "the sky did not give
us enough stars" from "plenty of stars, so the scale or the solver is wrong" —
a distinction that is pure guesswork after the fact otherwise, and guesswork is
what previously sent a night's debugging in the wrong direction.

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

**Nothing solves, from anywhere in the sky.**
Check the focal length first — it is the most likely cause by a wide margin, and
it fails *completely* rather than intermittently. The solver logs what it used
and where it came from (`[polaris-align] focal from EXIF: 70.0000mm`). Compare
the reported `pixscale_arcsec` against what your lens should give: 206265 ×
(sensor_mm / image_width_px) / focal_mm. Failed frames are kept under
`/app/sd/polaris-astro/failed/` with the star count, which separates "not enough
stars" from "plenty of stars, wrong scale".

**"Shot failed", and then the camera will not capture until it is power-cycled
and the USB replugged.**
Something fired a capture at the device during astro mode. It does not accept an
externally-initiated capture there: opcode 264 is ignored, and the 272 lapse
sequence is refused *while still establishing a lapse task that nothing aborts*
— that is the wedge, and why only a USB re-enumeration clears it. The autosolve
daemon issues no capture commands at all for this reason. To solve a full frame,
take the shot in the Benro app and run `solve-now.sh --latest`.

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

## Measured mount performance (2026-08-19, after a fresh calibration)

Numbers worth having, because two earlier estimates in this project were wrong.

### Pointing: ~6-9 arcsec

Five `goto --no-track` moves, commanded vs arrived, tracking OFF:

| commanded | error alt | error az |
|---|---|---|
| alt 22.6477 az 204.9244 | +4.8" | -2.7" |
| alt 21.1489 az 207.9239 | +13.7" | +2.0" |
| alt 23.6527 az 203.9242 | +8.0" | +10.4" |
| alt 22.8548 az 205.1268 | +11.6" | +6.9" |
| alt 25.8580 az 202.6284 | +8.2" | +9.8" |

**mean |error|: alt 9.3", az 6.4"**, and it holds position to 0.1" over 5 s.

### Tracking: Dec excellent, RA drifts ~4.2 arcsec/s

Over 20 s with tracking on:

```
Dec drift  -0.7"      <- essentially perfect
RA  drift +83.2"      <- 4.2"/s, about 28% of the sidereal rate
```

That RA drift is a residual alignment error showing up as a slightly wrong
tracking rate. Over a 60 s exposure it is ~250" of trailing, which is precisely
what the auto-guider exists to remove (measured holding drift under 61").

### TWO EARLIER ESTIMATES WERE WRONG -- how, so it is not repeated

**"~90 arcsec pointing error" was measurement error, not the mount.**
It was measured (a) with TRACKING ON, reading the pose seconds after arrival, so
sky motion was counted as pointing error, and (b) against an alignment CORRUPTED
by ConformU's sync tests, which sync to positions deliberately minutes of RA
off. `goto` has `--no-track`; it was not used. Real accuracy is ~10x better.

**Running a conformance suite against a live mount corrupts the alignment.**
Conform verifies a driver honours arbitrary sync requests; this mount honours
them literally. After such a run the mount slewed at 587"/s chasing a false
model and pointed visibly too high. Recalibrate afterwards, or point conformance
runs at `polaris-sim`.
