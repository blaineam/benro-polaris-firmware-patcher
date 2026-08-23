# The capture path: getting the RAW onto the camera's card

## The problem

Today the loader's `STAGE2_TETHER_CAPTURE` shim rewrites Canon's
`capturetarget` to **Internal RAM** on every `gp_camera_set_single_config`. The
shot then streams to the Polaris and both JPEG and RAW land on the Polaris
microSD — but **the camera's own card gets nothing.** For an astro session where
the RAW is the deliverable, that is the wrong trade.

The shim exists because, with `capturetarget = Memory card`, the post-capture
event never arrived and the download hung — the original stock-firmware symptom
in a new guise.

## MEASURED ON HARDWARE (2026-08-18)

Ran it: set `STAGE2_TETHER_CAPTURE=0` so the shim stopped forcing Internal RAM,
restarted `pgphoto`, fired a capture, and watched `/app/Clog.txt`:

```
capture_image[3084]: ----ARG_TRIGGER_CAPTURE1  0
capture_image[3109]: ------capture shutterSpeed 0.016667 s  delaymax 7030 ms
capture_image[3127]: ----GP_EVENT_CAPTURE_COMPLETE          <- shutter fired, +0.5 s
capture_image[3153]: ----capture image timeout              <- +7.4 s
captureImage[1579]: ----captureImage ret -1
```

So the shutter **does** fire and the camera **does** report
`GP_EVENT_CAPTURE_COMPLETE`; what never arrives is the file. No new file
appeared in `/app/sd/normal/` in 60 s, and the app was told
`state:-1` — the historical hang, reproduced on demand.

**The important number is `delaymax 7030 ms`.** Benro's own capture wait is
about seven seconds, scaled from the shutter speed (here 1/60 s). A 45 MP
RAW+JPEG written to the card and then enumerated over PTP can easily take
longer than that. So this may be less "the event is lost" than **"we stop
listening too early"** — which makes the polled-fetch design below more likely
to work, and gives it a measured budget instead of a guess.

## Why the event goes missing — three candidates

The three below remain the plausible mechanisms; the measurement above narrows
but does not yet decide between them:

1. **A newer event opcode.** The R5 Mark II is a 2024 body. Canon's newer bodies
   emit `PTP_EC_CANON_EOS_ObjectAddedEx64` for card writes (the 64-bit variant
   that carries large-file offsets). If the 2.5.34 ptp2 driver only recognises
   the 32-bit `ObjectAddedEx` for this body, the card-mode event is parsed as
   unknown and dropped, and `camera_canon_eos_capture` waits out its timeout.
   RAM-mode capture takes the `ObjectTransfer` path, which is untouched.
2. **Someone else drains the event queue.** `pgphoto` is gphoto2's CLI *plus* a
   fork of mjpg-streamer with a Benro `input_ptp2` plugin driving live view. If
   that live-view loop calls into the EOS event pump on its own thread, it will
   consume the `ObjectAdded` event before the capture path sees it — a race that
   RAM-mode capture survives because the image comes back inline.
3. **Event mode not re-armed.** Canon requires `SetRemoteMode`/`SetEventMode` to
   be in the right state for card-write notifications. The patcher already drops
   2.5.34's extra `SetRemoteMode` toggle in `camera_exit` for reliability; if the
   Polaris' own init leaves the event mode set for tethered capture only, card
   events would never be armed.

## Benro's actual capture loop (read from the DWARF reconstruction)

```c
t        = (shutterSpeed + 1.0f) * 100.0f;
delayMax = (int)(t + t + 500.0f);          /* units of 10 ms polls */
do {
    wait_and_handle_event(10, &evtype, 0); /* 10 ms poll */
    if (evtype == GP_EVENT_CAPTURE_COMPLETE && fileCount > 0) goto download;
    if (evtype == GP_EVENT_FILE_ADDED) { fileCount++; delayCount = 0; }
    delayCount++;
} while (delayCount <= delayMax && !cancelled);
if (fileCount < 1) { "capture image timeout"; ret = -1; }
```

Two consequences, both measured:

**Benro uses a different libgphoto2 API than a desktop user does.** They call
`ARG_TRIGGER_CAPTURE` — `gphoto2 --trigger-capture` — which fires the shutter
and returns, leaving the caller to poll for `GP_EVENT_FILE_ADDED`. A desktop
`--capture-image-and-download` instead lets libgphoto2 do the waiting
*internally* and hands back the file. Same library, same version (2.5.34 on both
the Mac and here), **different code path** — which is why card capture is
instant on a Mac and times out on the Polaris.

**The timeout scales with shutter speed:**

| exposure | Benro's wait |
|---|---|
| 1/60 s | **7.0 s** |
| 5 s | 17 s |
| 30 s | 67 s |

The 7 s budget measured above is the worst case, from testing at 1/60 s in
daylight. It is NOT a fix to rely on long exposures having a longer budget: the
solver wants short frames, and a capture path that only works above some
exposure threshold is not a capture path.

## The design: stop depending on the event

Diagnosing which of the three it is needs the camera. But we do not have to
know, because a fix that works for all three is available: **treat the event as
an optimisation, not a requirement.**

The refined mechanism, now that Benro's loop is known: **synthesize the event
they are waiting for.** Our loader already trampolines the public libgphoto2
API, so it can wrap `gp_camera_wait_for_event`:

```
  gp_camera_trigger_capture(...)      <- wrapper notes: a capture is pending,
                                         and snapshots the newest file on the card

  gp_camera_wait_for_event(...)       <- wrapper calls the real one first
    real event returned  ->  pass straight through          (fast path, unchanged)
    GP_EVENT_TIMEOUT     ->  every Nth call, list ONE folder on the card;
                             if a file is there that was not before, return
                             GP_EVENT_FILE_ADDED with its CameraFilePath
```

Benro's loop then sees `GP_EVENT_FILE_ADDED`, increments `fileCount`, resets
`delayCount`, and falls into its **own** download path unchanged. We are not
replacing their capture logic — we are supplying the one fact it is missing.

Why this works at **any** camera setting: the file lands on the card within a
second or two of the shutter regardless of exposure, and the loop always polls
for at least 7 s. The failure was never that the file was slow; it was that
nobody announced it. A filesystem poll does not care whether the event exists.

The camera writes to its own card either way — that part never depended on the
event. Only *our copy* did.

### Why polling is affordable here

The eager full-card scan that this project deliberately removed (`ARG_LIST_FILES`
at connect) was recursive over every folder — minutes on a full card. This is
not that. We list **one** folder — the newest `DCIM/1xxCANON` — and only compare
names. When the folder rolls over (100CANON → 101CANON) and no new name appears,
we re-list the folder set once. Typical cost: a single PTP `GetObjectHandles` on
one folder, tens of milliseconds.

### Which APIs it touches

All public LGPL libgphoto2 entry points the loader already trampolines — no
firmware code, no new symbols, consistent with the existing shims:

| call | why |
|---|---|
| `gp_camera_set_single_config` / `gp_camera_set_config` | stop forcing Internal RAM; let Memory card through |
| `gp_camera_capture` | wrap: on failure or empty path, run the fallback |
| `gp_camera_wait_for_event` | wrap: convert a timeout into a filesystem poll |
| `gp_camera_folder_list_files` / `_list_folders` | find what is new |
| `gp_camera_file_get` | fetch the new file to the Polaris |

### Behaviour matrix

| mode | camera card | Polaris SD | who uses it |
|---|---|---|---|
| `STAGE2_TETHER_CAPTURE=1` (today's default) | *nothing* | JPEG + RAW | plate-solve frames: throwaway, no card wear, no shutter-count cost on the card |
| `STAGE2_CARD_CAPTURE=1` (new) | **JPEG + RAW** | downloaded copy | real shooting |

Those are complementary, not competing: the solver wants a small disposable
frame, the photographer wants the RAW on the card. The end state is that the
alignment loop asks for RAM capture and everything else asks for card capture.

## Implemented (shims #4 and #5 in `stage2_loader.c`)

Both entry points were already in the 64-symbol trampoline set, so no new
machinery was needed:

| shim | what it does |
|---|---|
| `gp_camera_trigger_capture` | records the card folder and its file count at the moment the shutter fires — one folder listing, never a recursive walk |
| `gp_camera_wait_for_event` | passes real events straight through; on `GP_EVENT_TIMEOUT` polls that one folder at most ~2×/s and, if a file appeared, returns `GP_EVENT_FILE_ADDED` with its `CameraFilePath` |

`STAGE2_CAPTURE_DEBUG=1` additionally logs every event with `t+` timings
relative to the shutter, which answers the outstanding question — whether the
real event is **late** or **absent** — at zero behavioural cost.

Fail-open at every step: no folder found, no listing, allocation failure or any
error hands back exactly what the real call returned, which is today's
behaviour.

## Rollout, and why the new path ships OFF

`stage2_loader.c` is a hardware-validated artifact — `docs/TESTED.md` records its
md5 (`74f681de…`) as part of the reproducibility table. Any edit changes that
binary, so:

- the new behaviour is gated behind **`STAGE2_CARD_CAPTURE`, default off**; with
  it unset the loader behaves exactly as the validated build does;
- the recorded md5s must be **re-measured and re-recorded** the first time a
  changed loader is flashed and confirmed;
- the wrapper fails **open**: any error inside the fallback path returns the
  original call's result, so the worst case is today's behaviour, not a hang.

## How it gets verified

1. **On the bench, without a camera:** libgphoto2 ships a virtual PTP camera
   (`camlibs/ptp2/vcamera.c`, the `vusb` port) used by its own test suite. The
   fallback's logic — snapshot, capture, poll, diff, fetch — can be exercised
   against that, including the "event never arrives" case, by simply not
   delivering the event.
2. **On the device, with the camera:** the acceptance test is one line — fire a
   shot and confirm the RAW+JPEG are on the camera's card *and* a copy is on the
   Polaris SD, with no hang. That is the first thing to try once the R5 II is
   attached.

## HARDWARE RESULT (2026-08-18) — card-target capture WORKS

Verified on a physical Canon EOS R5 Mark II with `STAGE2_TETHER_CAPTURE=0`
(card mode) and the storage shim on, loader otherwise stock behaviour:

- Shutter fires immediately, at normal speed.
- **Images are written to the camera's own CF Express card** (confirmed by the
  maintainer on the camera).
- **Full-size copies land on the Polaris SD card**: 50 MB `.cr3` + 10 MB `.jpg`
  pairs in `/app/sd/Lapse/class_NN/`. Not truncated, not empty.

This satisfies the project requirement that capture must write to the camera's
internal card *in addition to* downloading to the Polaris SD card.

### The one remaining defect: the app is never notified

Pure-observation event trace of a single capture (shim logging only, no
behaviour change):

```
t+0.581s  event=4   GP_EVENT_CAPTURE_COMPLETE
t+0.640s  event=0   UNKNOWN
t+2.162s  event=0   UNKNOWN   (and only UNKNOWN thereafter)
```

`GP_EVENT_FILE_ADDED` (event=2) is **never delivered** — Canon's
`ObjectAddedEx` is not translated by the fresh 2.5.34 ptp2 camlib in card mode.
The bytes arrive anyway; the Benro app just never learns the photo exists, so it
spins and then reports "shot failed" over a capture that in fact succeeded.

**The remaining work is a notification bug, not a transfer bug.**

### Two dead ends — do not repeat them

Both were built on the false premise that the *file* was missing:

1. **Wait-stretch** (re-poll `wait_for_event` for ~250 ms after
   `CAPTURE_COMPLETE`). Harmless but pointless: the event never comes, so
   stretching the window cannot surface it.
2. **Filesystem poll** (walk the camera's storage after capture to find the new
   object). **Actively harmful** — it contends for the single PTP session.
   Observed on hardware: camera card LED went solid-read, the app hung
   indefinitely, and one test double-fired the shutter. Left in the tree behind
   `STAGE2_CARD_POLL=1`, default OFF. Leave it off.

### Method note

Both dead ends were pursued because "no `FILE_ADDED` event" was read as "no
file", and `ls /app/sd/Lapse/` was never run. One `ls` refuted several hours of
work. **Confirm the artifact exists on disk before interpreting event traces.**

## SOLVED (2026-08-18): card + host capture, hardware-verified

Both destinations at once — image on the **camera's CF Express card** AND
downloaded to the **Polaris SD card** — on a Canon EOS R5 Mark II.

### Root cause (two upstream bugs, compounding)

Canon's `EOS_CaptureDestination` (0xD11C) is a **bitmask**, not an enum:

```
0x1 = CFexpress    0x2 = SD slot 2    0x4 = host
0x5 = CFexpress|host      0x6 = SD|host
```

The R5 II advertises all five. Upstream 2.5.34 never surfaces them.

**Bug 1 — card value picked by "first non-host".** `camera_canon_eos_update_capture_target()`:

```c
if (SupportedValue[i].u32 != PTP_CANON_EOS_CAPTUREDEST_HD) { cardval = ...; break; }
```

lands on `0x1` (card only). With the host absent from the mask the camera never
emits `ObjectAdded`, so libgphoto2 has nothing to announce, and Benro's lapse
task times out at `delayMax` (~7.03 s) → `state:-1`. No amount of event-handling
work can fix this: the host is simply not a destination.

**Bug 2 — host capacity only declared for the exact-host case.** Setting `0x5`
alone yields `0x2019 PTP Device Busy` on Full-Press (`-110 I/O in progress`),
because `ptp_canon_eos_pchddcapacity()` — the "host has room" handshake — is
gated on `ct_val.u32 == PTP_CANON_EOS_CAPTUREDEST_HD`, i.e. exactly `4`. It is
*also* nested inside `if (ct_val != CurrentValue)`, so once the property is
already `0x5` from a previous run the declaration is skipped a second way and
the camera can never recover on its own.

### Fix (both in `container/dbg_patch.py`, applied to `camlibs/ptp2/config.c`)

1. Log all supported destinations; honour `POLARIS_EOS_CAPTUREDEST` to force one.
2. Declare host capacity whenever the host **bit** is set, independent of the
   "value changed" guard.

Capacity is declared **only when `AvailableShots` reads 0**. Declaring on every
trigger drains the EOS event queue live view feeds from — that regressed live
view on hardware and was caught immediately. The `AvailableShots` wait is also
**bounded** (~2 s); upstream's is `while (1)`, and `polestar_app` watchdogs
`pgphoto` at ~5 s, so an unbounded spin crash-loops the daemon.

### Verified on hardware

```
POLARIS capturedest supported[0..4] = 0x1 0x5 0x2 0x6 0x4
POLARIS capturedest OVERRIDE -> 0x5
[stage2] capture: t+6.561s event=2 ret=0      <- GP_EVENT_FILE_ADDED (first all day)
code[264] state:4                              <- downloaded
```

Live view works. RAW stays on the camera card; only the ~9.4 MB JPEG crosses to
the Polaris, which is the desired split (the solver only needs the JPEG).

### Announcement latency: ~0.94 s (the "6.1 s" was an instrumentation artifact)

Four consecutive captures announce at **t+0.935 / 0.962 / 0.940 / 0.974 s**,
all `state:4`, no failures. Against `delayMax` 7030 ms that is ~6 s of headroom,
so no `delayMax` patch and no wait-stretch are required.

**Do not enable `STAGE2_GPLOG` for normal use.** The gp_log bridge registers at
`GP_LOG_ALL`, where libgphoto2 logs every PTP packet, and the callback runs six
`strstr()` calls per line. On the Hi3559V200 that cost ~5.5 s per capture and
pushed announcements to 6.5-6.8 s -- right onto the 7.03 s `delayMax`, which is
what made shots fail intermittently. The bridge is a debugging tool only.

Two conclusions were drawn from logging-inflated numbers and are WRONG:
  * "the camera holds the object ~6 s and nothing on our side can help" -- no,
    that was the logging.
  * the ~86 polls/sec figure -- that measured a system throttled by its own
    instrumentation.

`STAGE2_CARD_CAPTURE` (wait-stretch) was enabled to buy margin against the
inflated latency and **broke captures outright on hardware**. It is not needed
at 0.94 s. Leave it unset, together with `STAGE2_CARD_POLL`.

### Superseded analysis (kept: it is why the above is worded so strongly)

The bus is **idle** for 6.1 s between the last property event (t+0.44 s) and
`FILE_ADDED` (t+6.56 s); the download itself completes in the same millisecond
as the announcement. So this is camera-side latency, not transfer time, and
optimising the download path would gain nothing.

Margin against `delayMax = ((shutter+1)*100*2+500)*10ms` is ~470 ms at 1/60 s.
**This gets safer for astro, not worse** — `delayMax` scales with exposure, so
long subs have far more headroom. Fast shutter speeds ride the edge.

Untested: whether the 6.1 s is the RAW card write. If so, JPEG-only shooting
would cut it sharply — worth measuring before optimising anything else.

## Auto-solve verification (2026-08-18), closed loop, no sky required

Two stages, both on hardware, both with known answers.

### Stage 1 -- render then solve, no mount, no camera

`polaris-skysim` renders a real star field from the index files; the solver must
recover the position it was given. At **960x640, the live-view resolution**:

| rendered | solved | error | time |
|---|---|---|---|
| 83.8, -5.2 | 83.800489, -5.206315 | 22.8" | 4 s |
| 202.5, 47.2 | 202.498551, 47.194369 | 20.6" | 3 s |
| 10.7, 41.3 | 10.697905, 41.293083 | 25.5" | 1 s |
| 279.2, 38.8 | 279.204279, 38.789531 | 39.6" | 2 s |

So solver, index selection and scale math are sound at live-view scale. What is
still unproven is only whether REAL stars register brightly enough in a ~1/30 s
live-view frame -- that is a photometry question, not a geometry one.

### Stage 2 -- inject a known pointing error, check it comes back

With the mount aligned: read its physical pose, render the sky at *pose + 5 deg
azimuth*, solve that, and ask `align` for the correction.

```
injected                5.000000 deg azimuth
az_error_deg            5.007804        <- 0.008 deg (28") from truth
alt_error_deg          -0.009018        <- ~0, correct: azimuth only was injected
solved   RA 110.980766  Dec -20.019396
rendered RA 110.980493  Dec -20.014260  <- agree to ~3.6"
```

That validates the whole chain end to end: pose -> error -> render -> extract ->
solve -> coordinate conversion -> correction.

### Known gap: 518 is silent at track:3

`align` derives the correction from the mount's pose (518). At **track:3**, the
never-aligned state after a cold boot, 518 does not answer:

```
$ polaris-mount pose        # track:3
no 518 pose message arrived
```

Once an alignment has completed it answers normally (verified above). So
auto-solve works for a RE-alignment but not for the first alignment after
power-on -- which is exactly when people calibrate.

Fix direction: fall back to **517** (raw motor angles), which always answers.
517 is ground truth and absolute; 518 is frame-relative and gated. Do not
confuse them -- an earlier session drew a wrong conclusion by reading 518 across
a frame change.

An earlier note in this session called this "blocked". That was an
overgeneralisation from a single failed command in a single mount state.

---

## Astro mode will not accept an externally-initiated capture

Worth stating plainly, because it has now cost two rounds of debugging:

- **Opcode 264** (single shot) is **ignored** in astro mode.
- **The 272 step:1/2/3 lapse sequence** worked exactly once and was refused
  afterwards while tracking was running — but a refused 272 **still establishes a
  lapse task** that nothing ever aborts. That is the wedge: the app reports
  "shot failed", live view keeps streaming (separate path, which is what makes
  it look survivable), and the camera will not capture again until it is
  power-cycled *and* the USB replugged, because only a USB re-enumeration clears
  it.
- With `photoCnt:-1` that lapse is **unbounded** — it fired 28 unwanted shutter
  actuations during development.

So the solver never fires the shutter during calibration. Live view is the only
frame source there. For a full-frame solve, take the shot in the Benro app and
run `solve-now.sh --latest`, which is what the `--latest` and `--wait` modes
exist for.

---

## THERE ARE TWO ptp2.so, AND ONLY ONE IS LOADED

The patcher installs the camlib to **both** of these:

```
/app/lib/stage2/libgphoto2/2.5.34/ptp2.so
/app/lib/libgphoto2/2.5.27.1/ptp2.so      <-- THIS is the one that gets loaded
```

`pgphoto`'s wrapper exports `CAMLIBS=/app/lib/stage2/libgphoto2/2.5.34`, so the
stage2 path looks like the live one. It is not: pgphoto's compiled-in core
resolves `2.5.27.1` first, and that is what ends up mapped.

Check with `/proc/<pid>/maps`, never by md5-ing the file you just copied:

```sh
P=$(ps | grep pgphot[o] | awk '{print $1}' | head -1)
grep ptp2.so /proc/$P/maps
```

Three consecutive hand-installed fixes were tested and "verified" against the
stage2 copy while the device ran the 2.5.27.1 one — matching md5s, matching
strings, and none of the code ever executing. A reflash would have worked, since
the patcher writes both. **A hand install must write both paths.**

---

## Cold start: the camera does not answer PTP for minutes

Reported: after a cold start (camera power-cycled or USB replugged) live view
works immediately, but every shot fails for minutes, then starts working.

What the log actually shows — and this took three wrong diagnoses to reach:

```
checkGphotoTask: pgphoto is exit, reboot it
gp_camera_set_abilities ('Canon EOS R5m2')
gp_camera_set_port_info ... usb:001,006
*** Error *** PTP Timeout
MsgFromCamera --> state:-10; manufacturer:none; model:none; storage:0
```

repeating every ~7 s. **`gp_camera_init` is timing out.** pgphoto exits,
`polestar_app` restarts it, and round it goes. The shutter errors
(`Full-Press failed (0x2019: PTP Device Busy)`) only appear on the attempts
where init got far enough to try, so they are a symptom, not the cause.

### What was ruled OUT, with evidence

* **The host-capacity declaration is not being skipped.** It runs and succeeds:
  `POLARIS host capacity declared for dest 0x5, tries=1`, and Full-Press fails
  anyway.
* **DeviceBusy being latched as success** was a real bug and is fixed (it now
  leaves the flag clear so the next capture retries), but it was not this.
* **Retrying Full-Press does not bridge it.** With a bounded retry in place the
  log reads `POLARIS capture full-press was busy: retries=9 result=0x2019` —
  eight retries over two full seconds, still busy. The wait is minutes, and the
  retry cannot be lengthened much: `polestar_app` watchdogs pgphoto at ~5 s.

### The remaining suspect

`resetUsb` is patched to return immediately (`mov r0,#0; bx lr`), deliberately
skipping `USBDEVFS_RESET`, to stop a re-enumeration storm that caused a
~3-minute stall on cold connect. Skipping the reset is exactly the kind of thing
that could leave a cold-started body unready to answer PTP. Testing it means
restoring the reset and checking whether cold start improves *and* whether the
old storm comes back. **Not yet tried.**

Evidence from a real cold start is preserved at
`/app/sd/coldstart-evidence.log` on the device.

### How to capture this yourself

`pgphoto`'s **stderr goes to `/app/Mlog.txt`**, not `Clog.txt` — and the device
truncates `Mlog.txt` constantly, so reading it after the fact gets nothing. Use
`polaris-logwatch` (20 ms, truncation-safe) to keep your own copy; the camera
flight recorder already does exactly this into
`/app/sd/polaris-astro/camera-roll.log`.

Enable `STAGE2_GPLOG=1` in `/app/bin/pgphoto` for the libgphoto2 detail, and
**turn it off afterwards** — it costs about 5.5 s per capture.
