# Upstream libgphoto2 bugs found on a Canon EOS R5 Mark II

Two bugs in `camlibs/ptp2/config.c`, function
`camera_canon_eos_update_capture_target()`, found against **libgphoto2 2.5.34**
on a Canon EOS R5 Mark II (USB `04a9:3314`).

Together they make it **impossible to select a combined capture destination**
(camera card *and* host) on this body: the first silently downgrades the
request to card-only, and the second makes an explicit combined value fail with
`PTP Device Busy`.

We currently carry these as local patches (`container/dbg_patch.py`).
**Not yet reported upstream — this file is the PR draft.**

---

## Background: `EOS_CaptureDestination` is a bitmask

`PTP_DPC_CANON_EOS_CaptureDestination` (`0xD11C`) is a bitmask, not an enum:

| value | meaning |
|---|---|
| `0x1` | camera card, slot 1 (CFexpress) |
| `0x2` | camera card, slot 2 (SD) |
| `0x4` | host (`PTP_CANON_EOS_CAPTUREDEST_HD`) |
| `0x5` | `1｜4` — CFexpress **and** host |
| `0x6` | `2｜4` — SD **and** host |

An R5 Mark II advertises **all five**:

```
prop d11c options changed, type 3, count  5 (EOS_CaptureDestination)
supported[0] = 0x1   supported[1] = 0x5   supported[2] = 0x2
supported[3] = 0x6   supported[4] = 0x4
```

libgphoto2's `capturetarget` config exposes only two choices
(`"Internal RAM"` / `"Memory card"`), so the combined values are unreachable
through the public API even though the hardware supports them.

## Bug 1 — the card value is chosen as "first entry that is not host"

```c
/* Look for the correct value of the card mode */
if (value != PTP_CANON_EOS_CAPTUREDEST_HD) {
    if (dpd.FormFlag == PTP_DPFF_Enumeration) {
        for (i=0;i<dpd.FORM.Enum.NumberOfValues;i++) {
            if (dpd.FORM.Enum.SupportedValue[i].u32 != PTP_CANON_EOS_CAPTUREDEST_HD) {
                cardval = dpd.FORM.Enum.SupportedValue[i].u32;
                break;
            }
        }
```

On this body the scan stops at `supported[0] = 0x1` — card-only. The host is
then **not a destination at all**, so the camera never emits `ObjectAdded`,
`camera_wait_for_event()` never returns `GP_EVENT_FILE_ADDED`, and no
application can learn that a picture was taken. Confirmed by trace: the shutter
fires, the image is written to the camera's card, `GP_EVENT_CAPTURE_COMPLETE`
arrives at t+0.57s, and no object event ever follows.

This is invisible to callers — the request to shoot "to card" appears to
succeed, and only the notification silently disappears.

## Bug 2 — host capacity is declared only when the destination is *exactly* host

Selecting `0x5` explicitly then fails: the camera rejects the shutter with
`0x2019 PTP Device Busy`, surfaced as `-110 GP_ERROR_IO`:

```
Canon EOS Full-Press failed (0x2019: PTP Device Busy)
camera_canon_eos_capture [library.c:4508]:
    'camera_trigger_canon_eos_capture (camera, context)' failed: 'I/O in progress' (-110)
```

Because the host-storage handshake is gated on **equality**:

```c
if (ct_val.u32 != dpd.CurrentValue.u32) {
    C_PTP_MSG (ptp_canon_eos_setdevicepropvalue (...));
    if (ct_val.u32 == PTP_CANON_EOS_CAPTUREDEST_HD) {   /* <-- exactly 4 */
        ret = ptp_canon_eos_pchddcapacity(params, 0x0fffffff, 0x00001000, 0x00000001);
        ...
        while (1) {   /* wait for AvailableShots > 0 */
```

With `0x5` the host **is** a destination but `ptp_canon_eos_pchddcapacity()` is
never called, so the camera believes the host has no room and refuses to shoot.

The same block also sits inside `if (ct_val.u32 != dpd.CurrentValue.u32)`, so
once the property is already `0x5` from a previous session the declaration is
skipped a second way and the camera cannot recover on its own.

### Suggested fix

Test the host **bit**, and declare capacity independently of whether the
property value changed:

```c
if (ct_val.u32 & PTP_CANON_EOS_CAPTUREDEST_HD) {
    /* host is a destination -- it must declare capacity */
}
```

Our local patch additionally declares capacity only when `AvailableShots`
currently reads `0`, because doing it on every trigger drains the EOS event
queue that live view feeds from (we regressed live view on hardware that way).

### Bonus: the `AvailableShots` wait is unbounded

```c
while (1) {
    C_PTP (ptp_check_eos_events (params));
    C_PTP (ptp_canon_eos_getdevicepropdesc (params, PTP_DPC_CANON_EOS_AvailableShots, &dpd));
    if (dpd.CurrentValue.u32 > 0) break;
}
```

If the body never reports free shots this spins forever inside a camera
operation. We bound it (~2s) because our host watchdogs the process at ~5s.
Worth bounding upstream too.

## Bug 3 (separate, not required for the above) — use-after-free in `camera_wait_for_event`

`camlibs/ptp2/library.c`, `PTP_EOSEvent_ObjectAdded` branch (~line 6795):

```c
case PTP_EOSEvent_ObjectAdded: {
    GP_LOG_D ("object added: handle 0x%x, name %s", ...);
    ptp_free_eos_event(&eos_event);                       /* freed here */
    ...
    if ((eos_event.type == PTP_EOSEvent_ObjectInfoChanged) && ...)   /* read after free */
    ret = add_object_to_fs_and_path (camera, eos_event.u.object.Handle, path, context);
```

The event is freed and then its members are read. `Handle` is an integer and
survives in practice, which is likely why this has gone unnoticed, but
`ptp_free_eos_event()` releases the heap `Filename`/`Keywords` of the embedded
`PTPObjectInfo`. The sibling code in `camera_capture()` (~line 4538) gets the
ordering right — it frees **after** use — so the two call sites disagree.

## Reproduction

Canon EOS R5 Mark II, libgphoto2 2.5.34:

1. `gphoto2 --set-config capturetarget=card`
2. `gphoto2 --capture-image`
3. Observe: image is written to the camera card; no `GP_EVENT_FILE_ADDED` is
   ever delivered to a `camera_wait_for_event()` caller.
4. Force `0xD11C` to `0x5` → shutter fails with `0x2019 PTP Device Busy`.
