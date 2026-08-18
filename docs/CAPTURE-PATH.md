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

## The design: stop depending on the event

Diagnosing which of the three it is needs the camera. But we do not have to
know, because a fix that works for all three is available: **treat the event as
an optimisation, not a requirement.**

```
  set capturetarget = Memory card        (the user's RAW lands on the card)
  remember the newest file on the card   (one folder listing, cheap)
  fire the shutter
  ├─ the normal path returns a file  ->  pass it straight through   (fast path,
  │                                      byte-identical to today's behaviour)
  └─ it times out                    ->  poll the card for a file that was not
                                         there before, fetch THAT, and hand it
                                         back as if the event had arrived
```

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
