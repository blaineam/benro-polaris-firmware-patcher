# What the Benro Connect app actually does

A feature and UX inventory of the **Benro Connect** Android app, read out of its
decompiled sources, written so you can rebuild its control surface as a
self-hosted web app served *from* the Polaris without ever opening the APK
again.

**Source of record.** `BenroConnect_1727595281455.apk`, `package
com.snoppa.libra`, `versionName v3.0.30`, `versionCode 240930`, `minSdk 26`,
`targetSdk 34` (`AndroidManifest.xml:1-12`). Decompiled Java lives in
`.../BenroConnect_decompiled/sources/com/snoppa/`; the resource tree
(`res/layout/*.xml`, `res/values/strings.xml`) was extracted separately with
`jadx --no-src` because the checked-in `resources/` directory is empty.

**Every quoted label below is the verbatim English string** from
`res/values/strings.xml`. Reuse them. They are the product's vocabulary, and
matching them is most of what makes a third-party client feel like the same
product rather than a clone.

Where this document says *inferred*, the code does not state it outright and I
reasoned from surrounding evidence. Everything else is what the code does.

## Contents

1. [Screen inventory](#1-screen-inventory) — every activity, dialogue and mode panel
2. [The main control screen](#2-the-main-control-screen) — joysticks, camera strip, status, hints
3. [The creative programs](#3-the-creative-programs) — PANO, ASTRO, SUN, TIMELAPSE, PATH-LAPSE, HDR, Holy Grail, FOCUS STACK, FREE PROGRAM
4. [Settings and device management](#4-settings-and-device-management) — including the media library
5. [UX conventions worth copying](#5-ux-conventions-worth-copying) — and what is clumsy
6. [Prioritised port list](#6-prioritised-port-list) — core / valuable / skip, and the phone-only dependencies

Each program section ends with a **"what a naive port gets wrong"** list. Those
are the parts worth reading even if you skip the rest — they are unit
inversions, off-by-ones and dead code that look correct until hardware
disagrees.

## Scope: what was excluded

The APK is one shell hosting three device families:

| Package | Product | Included here |
|---|---|---|
| `com.snoppa.libra` | the app shell itself — welcome, home, device list, global settings | yes |
| `com.snoppa.polaris` | **the Polaris tripod head** | yes, in full |
| `com.snoppa.theta` | a different Benro/Snoppa device family — **not** a Ricoh Theta and not a 360 camera | **excluded** |
| `com.snoppa.application` | shared infrastructure: sockets, ijkplayer, the embedded Sky Map star engine | included where the Polaris uses it |

`libra` is the *applicationId*, not a separate device: `WelcomeActivity`,
`HomeActivity`, `MyDeviceActivity` and `SettingActivity` all live under
`com.snoppa.libra.activity` and are the shell every device module launches from.

**Excluded on purpose — seven activities**, all `com.snoppa.theta.activity.*`:
`ThetaActivity`, `ThetaPhotoAlbumActivity`, `ThetaGifSyncActivity`,
`ThetaMediaExtraActivity`, `ThetaLocalMediaExtraActivity`,
`ThetaPreviewThumbnailActivity`, `ThetaBrightnessTrendActivity`. Theta has its
own shooting-mode set (`com/snoppa/theta/dialog/SelectShootingModelDialog.java:126-130`
— photo, video, static time-lapse, HDR, focus stack) which is a strict subset of
the Polaris's and is *not* what the Polaris firmware speaks. Nothing Polaris-only
was dropped.

Two caveats about "Theta", because the name misleads. It is a **Benro hardware
family**, identified by the BLE name prefixes `theta_live_` and `theta_oms_`
(`application/constant/theta/ThetaCMD.java:6-7`) — the second of those is the
**Optical Matrix Sensor Module** that the Polaris's own Holy Grail mode depends
on (§3.7), so the two products are not unrelated. And **some Theta copy leaks
into Polaris screens**: the Polaris `"Shooting Tips"` sheet is written entirely
in Theta's voice (§5.7). Where a Theta string is the *only* evidence for a claim,
this document says so.

## How the app talks to the head

Relevant because it decides what a browser-hosted UI can and cannot do cheaply.

| Channel | Endpoint | Source |
|---|---|---|
| Control | TCP `192.168.0.1:9090`, a persistent socket | `application/oksocket/SocketHelper.java:104-105` |
| Live view | `http://192.168.0.1:8080/?action=stream` — **MJPEG**, mjpg-streamer convention | `application/oksocket/SocketHelper.java:107` |
| Media / resources | `http://192.168.0.1/`, with WebDAV at `/dav` | `SocketHelper.java:106`, `polaris/singleton/PolarisSyntheticHelper.java:934` |

The live view being plain MJPEG over HTTP is the single most useful fact in this
document: an `<img src="http://…:8080/?action=stream">` renders it in any browser
with no player, no WebRTC and no transcode. The Android app routes it through
ijkplayer (`PolarisIjkVideoPlayView`) — a full ffmpeg stack, complete with an
`rtsp_transport=tcp` option it sets unconditionally
(`application/ijkplayer/PolarisIjkVideoPlayView.java:1610`) — but nothing in the
Polaris path needs any of that.

Command vocabulary is `SP_*` methods on
`polaris/singleton/PolarisOrderCommunication.java` (3,865 lines, ~175 commands),
with opcodes in `application/constant/polaris/PolarisCMD.java`.

**This document is the UX and feature layer, not a protocol reference.** It
names commands only where they explain a behaviour. For the wire itself —
framing, reply codes, the command index, sequencing and the danger list — see
the companion **[`docs/APP-PROTOCOL.md`](APP-PROTOCOL.md)**, plus
[`docs/HOW-IT-WORKS.md`](HOW-IT-WORKS.md) and
[`docs/NETWORKING.md`](NETWORKING.md).

---

# 1. Screen inventory

The app declares only **22 activities** — 10 Polaris, 5 shell, 7 Theta. The real screen surface is
in **64 full-screen dialogs** under `com/snoppa/polaris/dialog/` plus the **10
swap-in mode panels** under `com/snoppa/polaris/layout/shootingmodel/`. Almost
everything the user does happens as an overlay on the live-view screen, not as a
navigation push. Plan the web app the same way: one persistent live-view page,
panels sliding over it.

## 1.1 Shell screens (`com.snoppa.libra`)

| Screen | Orientation | Purpose | Reached by |
|---|---|---|---|
| `WelcomeActivity` | portrait | splash — just a logo, no UI | launch |
| `BenroConnectHomeActivity` | portrait | **the current shell**: a 3-tab `ViewPager2` | after welcome |
| `HomeActivity` | sensorLandscape | **legacy** shell, the old Polaris ViewPager | only if a stale preference is set |
| `MyDeviceActivity` | sensorLandscape | **legacy** saved-device list | legacy shell only |
| `SettingActivity` | sensorLandscape | **legacy** settings (1,383 lines) | legacy shell only |

`WelcomeActivity.java:35-47` branches on a `"polarisUI"` preference that
**defaults to false**, so `BenroConnectHomeActivity` is the live path and the
three `*Activity` screens below it are dead weight. Its three tabs
(`HomeActivityInitView.java:36-38`, labels in `Indicator.java:87-98`):

| Tab | Fragment | Contents |
|---|---|---|
| `"Connection"` | `HomeScannerFragment` | device discovery, connected-device card, gear → Settings, a `"Nearby Devices"` sheet |
| `"Media Library"` | `HomeMediaFragment` | media pulled off the head; empty state `"No Files"`; multi-select, delete, sync |
| `"Discover"` | `HomeDiscoveryFragment` | static: `"Operation Instructions"`, `"Software Updates"`, `"Product Updates"` — **ships with placeholder Chinese lorem and fake dates** |

The fragments are thin shells; the logic lives in `libra/fragments/impl/*InitView.java`
(`HomeScannerFragmentInitView` alone is 1,915 lines). Settings and My Device hang
off the gear icon on the `"Connection"` tab, both **password-gated**.

Note the orientation split: onboarding and password screens are **portrait**,
everything operational is **landscape**. The Polaris control screen is
landscape-only and assumes a two-thumb grip.

## 1.2 Polaris screens (`com.snoppa.polaris`)

| Activity | Orientation | Purpose |
|---|---|---|
| `MainActivity` | sensorLandscape | **the live control screen** — everything in §2 |
| `PhotoAlbumActivity` | sensorLandscape | media library on the head |
| `MediaActivity` | sensorLandscape | media viewer |
| `MediaExtraActivity` | sensorLandscape | grouped/derived media (stitch groups) |
| `MediaExtraItemActivity` | sensorLandscape | single item within a group |
| `FirstChangePasswordAcitivty` | portrait | forced password set on first connect |
| `ChangePasswordAcitivty` | portrait | change device password |
| `ForgetPasswordAcitivty` | portrait | recover via security question |
| `ServiceWebViewActivity` | portrait | in-app web content (help/terms) |
| `ServiceWebViewHorActivity` | sensor | same, landscape |

(The two `*Acitivty` spellings are the app's own typo, kept here so the names
grep cleanly.)

## 1.3 The mode panels

Ten user-selectable shooting modes, defined in one place —
`polaris/dialog/SelectShootingModelDialog.java:147-156` — with their integer
mode IDs and the exact labels the picker shows, listed in **display order**
(`:158-168`):

| Order | Label | Mode ID | Panel class |
|---|---|---|---|
| 1 | `PHOTO` | 1 | `OrdinaryPhotoLayout` |
| 2 | `HDR` | 6 | `HDRLayout` |
| 3 | `VIDEO` | 10 | `OrdinaryVideoLayout` |
| 4 | `PANO` | 2 | `PanoramaLayout` |
| 5 | `TIMELAPSE` | 4 | `StaticLapseLayout` |
| 6 | `PATH-LAPSE` | 5 | `DynamicLapseLayout` |
| 7 | `FOCUS STACK` | 3 | `FocusTrackLayout` |
| 8 | `ASTRO` | 8 | `StarrySkyLayout` |
| 9 | `SUN` | 7 | `SunLayout` |
| 10 | `FREE PROGRAM` | 9 | `PrecompileLayout` |

`HDR` is a hardcoded literal, not a string resource. The others resolve from
`photomodel`, `recoder_video`, `panoramicmodel`, `staticlapse`,
`dynamiclapsemodel`, `focusstackmodel`, `night_exposure`, `sunsetmodel`,
`free_programming`.

Two names mislead and are worth fixing in your port:

- **`FocusTrackLayout` is focus *stacking*, not subject tracking.** The class
  name is a leftover; every user-facing string says `FOCUS STACK`.
- **`PrecompileLayout` is `FREE PROGRAM`** — a user-authored motion program.
  "Precompile" appears nowhere in the UI.

Four modes are unavailable when the head is in Shutter (cable-release) mode
rather than USB mode: **HDR (6), FOCUS STACK (3), FREE PROGRAM (9), VIDEO (10)**
— `polaris/adapter/ShootingModelAdapter.java:23-25`. Tapping one shows
`"Please switch to the USB Mode to enable this feature"`
(`SelectShootingModelDialog.java:59`). Shutter mode also hides the Holy Grail
control and *adds* sub-second capture intervals that USB mode does not offer.

The picker itself is a bottom sheet: a `GridView` of icon-over-label cells
(`shootingmodel_item_layout.xml`) that slides up and down over 150 ms
(`SelectShootingModelDialog.java:174-176`).

---

# 2. The main control screen

`MainActivity` + `res/layout/activity_main.xml` (77 lines) + a 3,193-line
activity class. This is the screen the web app has to replace, so it gets the
detail.

## 2.1 Composition

The layout is a `RelativeLayout` with `android:keepScreenOn="true"` on a pure
black background, and **eleven sibling overlay layers stacked on one full-bleed
video surface**. Nothing is a separate page:

```
PolarisIjkVideoPlayView   ← live view, match_parent, the whole screen
  HistogramLayout         ← invisible by default
  GridLineLayout          ← gone by default
  AlignFrameView          ← gone; the ASTRO alignment frame — a pinch-zoom
                             magnifier, scale 1x-16x (AlignFrameView.java:14-19),
                             shown when align mode opens (MainActivity.java:2585)
  RockerLayout            ← the two joysticks
  shootingModelLayoutContainer  ← the current mode's panel swaps in here
  BatteryMemoryLayout     ← top-left status row
  ParameterItemLayout     ← camera settings strip
  HintLayout              ← centre hints, marginTop 50dp
  RightControlLayout      ← right-edge command rail
  TopControlHintLayout    ← top banner slot
  PreviewMediaLayout      ← gone; floating thumbnail + full preview
  AutoLevelLayout         ← levelling overlay
```

Landscape only. The geometry is a **game controller**: joysticks under both
thumbs, a command rail on the right edge, status top-left, camera settings along
the bottom-left.

## 2.2 The joysticks

`res/layout/rocker_ayout.xml` + `polaris/view/RockerView.java` +
`polaris/layout/RockerLayout.java`.

Two 176 dp circular sticks, inset 62 dp from the left and right edges,
vertically centred. Colours are baked into the layout: knob `#4dffffff`, frame
`#571c252f`, 2 dp lines, 15 dp knob radius.

**The sticks are single-axis, and which axis depends on whether the Astro Kit is
attached.** `RockerLayout.changeRockerUI(boolean hasTriaxial)` (`:221-238`):

| Astro Kit | Left stick | Right stick |
|---|---|---|
| absent | `drawType 0` — **tilt only**, vertical travel | `drawType 1` — **pan only**, horizontal travel |
| attached | `RockerViewLV`, `drawType 3` — **free pan+tilt**, latches to whichever axis you move first (`isPitch`) | `drawType 2` — **roll**, drawn with a 3x thicker line |

`RockerView.onDraw` (`view/RockerView.java:246-270`) confirms it: type 0
draws only the pitch line and circle, types 1 and 2 only the level line and
circle. **A naive port that ships two free 2-D pads is wrong** — on a 2-axis
head each stick is a constrained 1-D slider, and that constraint is what makes
single-axis framing moves possible one-handed.

### Interactions

- **Drag** — dead zone 20 px (`RockerView.deadzone`, `:26`). Travel maps to a
  continuous rate; the app emits `SP_GIMBAL_HADJ_SPEED` / `SP_GIMBAL_VADJ_SPEED`
  / `SP_GIMBAL_RADJ_SPEED` (`MainActivity.java:468-477`). It is a **velocity**
  control, not position. Release stops.
- **Haptic detents** — `VibrationEffect.createOneShot(10, 245)` as the knob
  crosses steps (`RockerView.clickEffects`, `:179-190`).
- **Double-tap a stick = recentre that axis** (`MainActivity.java:480-499`):
  tilt stick → `SP_GIMBAL_POS_RESET(2)`; pan stick → `RESET(1)`; roll stick →
  `RESET(3)`; the free stick → `RESET(1)` **and** `RESET(2)`. Cheap, discoverable,
  worth copying.
- **Single tap = switch to step mode.** 150 ms after a tap inside the knob
  radius (`RockerLayout.java:190-192`), both analog sticks are replaced by
  `ClickRockerView` D-pads, and a `GearSeekbar` (70 x 150 dp) appears beside the
  left one. Touching anywhere else in the rocker area reverts to analog
  (`:88-100`).

### Step mode and speed

In step mode each press emits `SP_SET_ROCKER_ADJUST(code, key, clickType,
speedLevel)` (`MainActivity.java:502-508`). Hold for 400 ms and it re-fires as
`long_click` and repeats (`RockerLayout.java:169-179`).

Speed is **1 to 5**, default **1** (`RockerLayout.rockerSpeedLevel = 1`,
`:54`). Changing it toasts, with distinct copy at the ends
(`MainActivity.java:511-522`):

- `"Fine-tuning Speed: 1 step (Min.)"` — `rocker_speed_low`
- `"Fine-tuning Speed: %1$d steps"` — `rocker_speed`
- `"Fine-tuning Speed: 5 steps (Max.)"` — `rocker_speed_high`

Note the speed gear belongs to **step mode only**; analog drag has no speed
selector, because travel is the speed.

## 2.3 The camera settings strip

`res/layout/parameteritemlayout.xml`, driven by
`polaris/layout/ParameterItemLayout.java` (1,644 lines). Bottom-left, two
states.

**Collapsed** — one pill (`cameraParameterItemView`) showing five icon+value
pairs. The layout's own placeholder text tells you the formatting:

| Field | Icon | Sample value | Notes |
|---|---|---|---|
| Shutter | `home_icon_shutter` | `1/40` | fraction, no unit suffix |
| Aperture | `home_icon_aperture` | `F2.6` | `F` prefix, no space |
| EV | `home_icon_ev` | `+1.6` | signed, one decimal |
| ISO | `home_icon_iso` | `40000` | bare integer |
| WB | `home_icon_wb` | `AUTO` | text or an icon (`wbValueImage`) |

Beside it, an AF/MF toggle rendered literally as `[ MF AF ]` — two chips inside
bracket glyphs.

**Expanded** — tapping the pill (`:250`) reveals `cameraParameterControlView`:
five labelled horizontal **scroll pickers** (`ParameterStringScrollPicker`),
tagged `S`, `F`, `EV`, `ISO`, `WB`, the last using bitmaps rather than text.
Which pickers are visible depends on the camera's exposure mode (`:460-496`) —
in shutter priority the `S` picker is `View.INVISIBLE`, not `GONE` (`:473`), so
the strip does not reflow. Copy that: **reserve the slot, grey the control**.

Scroll pickers, not steppers, is the right call — an ISO run of 100…40000 is
too long to step through, and a flick lands anywhere in it.

### Also in the strip

- **Bulb** — `BulbTimeSelectLayout`, appears when the shutter picker bottoms out
  into bulb.
- **Save camera parameters** — `ivCameraParamSave`, hidden by default.
- **Cable-release shutter time** — when the head is in Shutter mode there is no
  camera dialogue to read, so the user *declares* the exposure with three
  scroll pickers: **minutes / seconds / milliseconds**
  (`sp_minute`, `sp_second`, `sp_millisecond`), with a hint and a green coaching
  callout carrying a `"Do not show again"` checkbox (`no_longer_prompt`).
- **MF focus jog** — `mfParameterControlView`, six buttons in a row:
  `left_add_fast/middle/slow`, then `right_add_slow/middle/fast`. Press fires
  after 30 ms and **auto-repeats every 300 ms while held**
  (`:1370-1393`, `:1586-1633`). They map to a signed 7-position scale via
  `setFocusSpeed("1", n)`: near = `6,5,4`, far = `2,1,0`, with `3` the implied
  centre. Each press also gives a haptic tick.
- **HDR bracket indicator** — `fixHDRView`, three slots, shown only in HDR mode
  (`:976-980`).

## 2.4 The right-hand command rail

`res/layout/rightcontrollayout.xml`. A 74 dp column pinned to the right edge,
top to bottom:

| Control | Behaviour |
|---|---|
| `shootingModelIcomView` | current mode's icon; tap opens the mode picker sheet |
| `ivInnerSetting` | opens the on-screen settings sheet (§4.1) |
| `RecordButtonView` (centre) | **the shutter / record control** — see below |
| `PreRecordButtonView` | the *scheduled-start* variant — shown **only in `FREE PROGRAM`** (`RightControlLayout.java:178-189`: the record button is swapped for it when `currentShootingModel == 9`), where a run is booked for a future time rather than started now. `SUN` hides both buttons on entry and reveals the normal one through its own state machine (`:212-214`). |
| `albumIcomView` | media library |
| `backHomePage` | back to the shell home screen |
| `pauseIcomView` | pause a running program; a `"Pause"` label (`has_pause`) appears to its left while paused |

### The shutter is a slide, not a tap

`polaris/view/RecordButtonView.java:192-230`. The button is 54 x 149 dp — tall,
not round — because **you arm it by dragging your finger up the strip and
releasing above the top third of its height**. Release below that threshold
cancels and the knob springs back to `bottomBorder` (122/149 of the height).
Stopping a running capture uses the same upward slide from state 3. A haptic
fires only on commit.

During a long exposure `updateExposureTime(ms)` runs a 100 ms ticker that draws
a depleting curve around the button, ignoring anything under 1,000 ms
(`:102-131`). So the shutter control doubles as the exposure countdown.

This is a deliberate guard against a fat-finger motor or capture start, and the
intent is right. The *gesture* is not — see §5.

## 2.5 Status readouts

`res/layout/top_left_layout.xml` via `polaris/layout/BatteryMemoryLayout.java`.
A single horizontal row in the top-left, mixing read-only telemetry with live
toggles:

| Element | Kind | Notes |
|---|---|---|
| `connectModelChangeView` | **toggle** | Wi-Fi ↔ cellular transport |
| `iv_wifi_signal` | readout | signal bars |
| `bateryImage` + `bateryText` | readout | Polaris battery, rendered `"NN%"`; the icon steps at **>80 %**, **>20 %**, else (`:302-308`) |
| `memoryImage` + `memoryText` | readout | SD card free space, in MB, `"None"` when absent |
| `omsBateryImage` + `omsBateryText` | readout | the **Optical Matrix Sensor Module** battery, `"NN%"` — fed by `getOmsBatteryModel()` (`MainActivity.java:1954`), icon `light_sesion_icom`. Hidden when the accessory is absent (`:311-320`). **Not the Astro Kit**, which has no battery readout of its own. |
| `hidePreviewView` | **toggle** | hide the live view (saves bandwidth/power) |
| `ivAutoLevel` | **action** | trigger auto-level; when dimmed below 0.8 alpha, tapping toasts `"Auto Level can not be accessed in this feature."` |
| `humanRemoveView` | **toggle** | People Remover — toasts `"People Remover function is turned on."` / `"…is turned off."` |
| `ivCableReleaseMode` | indicator/action | USB vs Shutter mode |

Mixing toggles into the status row is compact and I would keep it, but the
affordances are invisible — nothing distinguishes the tappable icons from the
readouts until you tap one.

**People Remover** is a capture *modifier*, not a mode: it flips a flag on
`BenroAccount…polarisCamaraParameter.peopleRemove` and the head runs a
multi-frame median stack (`SP_REMOVE_PEOPLE_SHOT_START/_NUM/_END/_COMPLITE/_CANCLE`).
It refuses to toggle while recording (`:105-107`).

## 2.6 The top hint banner

`polaris/layout/tophint/TopControlHintLayout.java` — one banner slot at the top
of the live view, holding a `TopCommonHintLayout` built from *(id, icon,
message, action-label, action-icon)*. Thirteen registered hints, ids 0–12:

| id | Message | Action |
|---|---|---|
| 0 | `"SD Card Error, Please Format the SD Card"` | `"Format SD Card"` › |
| 1 | `"Please Connect to Your Camera"` | `"Help"` › |
| 2 | `"Do not adjust the rotation angle significantly, as this may cause you to lose the sun path line on the screen."` | `"Continue"` |
| 3 | `"Automatic positioning has been disabled. Do you want to enable it?"` | `"Turn on GPS"` › |
| 4 | `"To protect your device from being connected to by others, we recommend that you change your password on the settings page in a timely manner."` | `"Go to Create Password"` › |
| 5 | `"Celestial position is not yet aligned, please go to the star search list first and select a star [ GOTO ], this star will be used as an alignment star."` | (star icon) |
| 6 | `"Use the virtual joystick to move the selected star inside the red circle of the center of the screen, then click the Confirm button"` | `"Continue"` › |
| 7 | `"Reference target selected, if you want to use the tracking function, please tab Align"` | `"Continue"` |
| 8 | `"The power of the device is less than 20%, please charge the device in time."` | `"Continue"` |
| 9 | `"Please insert a memory card for the camera"` | `"Continue"` |
| 10 | `"Make sure the camera memory card is installed before use."` | `"Continue"` |
| 11 | `"Please set your Start Position by manipulating the joystick on the screen"` | (none) |
| 12 | `"Please set the interval and shots properly so the Polaris can reach the next position within the set time range."` | `"Continue"` |

This is the single best pattern in the app and the one most worth copying
wholesale: **every blocking condition is a non-modal banner carrying its own
fix**. Low battery threshold is **20%**.

## 2.7 Auto Level

`res/layout/layout_auto_level.xml`. A full-screen overlay with an animated
level line, the status text `"Leveling...."`, a success tick, and an `"Exit"`
button; on first use a green coaching callout points at the status-bar icon:
`"Tap to start Polaris Auto Level adjustment. Switch Auto Level on/off in
Overall Settings."` Commands: `SP_SET_AUTO_LEVEL_EN` / `SP_SET_AUTO_LEVEL_STATE`.

Auto Level is refused in dynamic modes (PATH-LAPSE and friends), where the app
tells the user to level the tripod by hand instead.

## 2.8 The floating preview

`res/layout/preview_media_layout.xml`. After each frame a thumbnail
(`littlePreview`) appears bottom-left; tapping expands it to a pinch-zoomable
`PhotoView` with a histogram overlay and three actions — `"Complete"`, a
histogram toggle, and delete. Delete opens an inline two-button sheet,
`"Delete"` / `"Cancel"`. Toggled by `"Floating Thumbnail"` in Overall Settings.

---

# 3. The creative programs

Ten modes, but they are not ten peers: `PHOTO` and `VIDEO` are the baseline, and
the other eight are programs that drive the head and the shutter together. Each
one lives in a panel swapped into `shootingModelLayoutContainer` over the live
view, so the picture is never lost while setting up.

Common to all of them:

- **Progress is a countdown of shots remaining, never "N of M".** No ETA is
  computed anywhere.
- **Cancellation is the shutter control again** — the same slide gesture, now
  meaning stop. Each mode has its own `SP_*_CANCLE` command.
- **Whether the head returns to where it started is a persistent per-mode
  setting**, not a per-run choice (§4.1).

## 3.1 PANO — panorama

Mode ID 2, `PanoramaLayout.java`. **Three sub-modes**, chosen by chips at the
top-left of the panel and persisted in `panoSelectType`
(`application/constant/ApplicationConstants.java:17-19`); default 0.

| Chip | Toast on switch | How bounds are set |
|---|---|---|
| (camera icon) | `"PANO"` | you dial per-shot angle and shot count on four wheels; no optics model at all |
| `PRO` | `"PANO PRO"` | you **fly the head** to a start and an end point; the grid is derived from sensor + focal length + overlap |
| `720` | `"720 PANO"` | bounds are fixed at **360° × 157°** (`PanoramaLayout.java:52-53`) |

There are two more, reached from `ASTRO` rather than `PANO`: **Astro Pano
normal** and **Astro Pano PRO** (`skyModeSelectType` 1 and 2,
`PanoSkyNorParamterLayout` / `PanoSkyProParamterLayout`). They are structural
twins of the two above with their own persisted fields, and the head treats them
identically — `MainActivity.clickStop()` sends the same `SP_PANORAMIC_CANCLE()`
for both (`MainActivity.java:814-818, 840-845`). Six panorama configurations in
total.

**Sequencing is row-major**: `row = i / hCount; col = i % hCount`
(`PanoramaLayout.java:1775-1796`) — sweep a full horizontal row, step in pitch,
sweep back.

### PANO (normal) parameters

`PanoNorNorParamterLayout.java`, `res/layout/pano_nor_nor_paramter_layout.xml`.
Two identical dial+wheel columns headed `"Horizontal"` and `"Vertical"`:

| Control | Label | Unit | Values | Default |
|---|---|---|---|---|
| step angle | `"Rotation"` | ° | `0, 0.1, 0.2, 0.4, 0.6, 0.8`, then `1…90` — 96 entries (`:338-352`) | index 1 = **0.1°** |
| shot count | `"Shots"` | count | `1 … floor(360/step)` (`:361-364`) | index 2 = **3 shots** |
| start corner | (icons) | — | 0 centre, 1 upper-left, 2 upper-right, 3 lower-left, 4 lower-right (`:203-234`) | **1, upper-left** |
| per-spot repeat | `"No. of photos per spot:"` | shots | **1–99**, two digit wheels; `00` corrects to 1 | **1** |
| per-spot interval | `"Shooting interal："` *(sic)* | s | **0–20**; any `2x` snaps to 20 | **0** |
| total | `"Total Shots:"` | read-only | `vCount × hCount × perSpot` (`:500-502`) | — |
| coverage | (bare value + `°`) | read-only | `step × count`, 1 dp | — |

The interval row is hidden entirely when per-spot count < 2 (`:128-135`).
Choosing a start corner toasts an instruction, e.g. `"Start shooting from the
top left corner with the current shooting position as the starting point."`
(`MainActivity.java:1418-1438`).

### PANO PRO parameters

A bottom bar of six taps (`res/layout/panoramalayout.xml`):

| Label | Meaning | Default |
|---|---|---|
| `"Shots · Rows/Rotation"` | read-out: `hCount/hStep°·vCount/vStep°` | — |
| `"Total Angle of View"` | opens start/end capture | **180° × 0°** (one row) |
| `"Sensor Size"` | 9 presets | **"Full Frame"** |
| `"Focal Length"` | 46-entry table | **24 mm** |
| `"Viewing Frame"` | `"Portrait"` / `"Landscape"` — an inline toggle, not a dialogue | **Landscape** |
| `"Overlap Percentage"` | 12 entries | **33 %** |

**Sensor presets** (`PanoramaLayout.java:1174-1191`, identical tables in the 720
and Astro paths): `"Full Frame"` 36×24, `"Med-Format"` 44×33, `APS-H （Canon）`
27.9×18.6, `APS-C` 24×16, `APS-C （Canon）` 22.3×14.9, `FoveonX3` 20.7×13.8,
`Micro4/3` 17.3×13.0, `Nikon EVIL` 13.8×10.38, `1 INCH` 12.8×9.6. Picker is a
4-column grid titled `"Select the sensor size of the camera"`.

**Focal lengths** are a fixed 46-entry list, not a range
(`PanoramaLayout.java:1221-1280`): 8, 9, 10, 11, 12, 14, 15, 16, 17, 20, 24, 28,
30, 35, 40, 45, 50, 55, 70, 85, 90, 100, 105, 110, 120, 135, 150, 180, 200, 250,
300, 350, 400, 450, 500, 600, 700, 800, 900, 1000, 1200, 1400, 1600, 1800, 2000,
3000 mm. A free-text escape exists — `"Enter Other Values >>>"` → `"Set"` — and
it is parsed with a bare `Float.parseFloat` with **no minimum, maximum or
validation** (`PaFoucsDialog.java:250-259`). Add bounds in your port.

**Overlap**: 15, 20, 25, 30, **33**, 35, 40, 45, 50, 55, 60, 65 % — step 5 with
33 spliced in. Default 33 %. The hint under the wheel changes with the value:
33 % → `"day-time panoramas"`, 40 % → `"Recommended overlap percentage"`, 50 % →
`"night-time panoramas"`. Dialogue title `"Choose the minimum overlap percentage
for each image"`.

### The grid computation

`PanoramaLayout.resetCalculate` (`:1416-1607`), worth reproducing exactly:

```
FOV°      = 2·atan(dim / (2·f))          # NormFovHelper.java:51-80
                                          # rounded to 2dp, float divisor
hStep     = hFOV − hFOV·overlapPct/100
vStep     = vFOV − vFOV·overlapPct/100
hCount    = ceil(|hSweep| / hStep + 1)
vCount    = ceil(|vSweep| / vStep + 1)    # then one row may be dropped, below
coverage  = min(|sweep| + FOV, 360)
```

FOV is floored at 0.1°. 720 mode uses the fixed 360/157 sweeps and then
**decrements both counts by one**, and skips the pitch clamp entirely
(`:1550-1606`).

### Setting the bounds (PRO only)

1. A one-time banner: `"Please set your Start Position by manipulating the
   joystick on the screen"`, with `"Do Not Show Again"`.
2. Tap `"Total Angle of View"`. The right rail, parameter strip and status row
   all hide (`MainActivity.java:1172-1183`). Hint: `"Set your initial position by
   moving the joystick on the screen and tap the Start button to save this
   spot."` Controls: a round green **`Start`** button and **`Cancel`**.
3. Joystick the head, tap `Start`. The button turns **red** and relabels to
   **`End`**; a 500 ms position poll begins and the grid recomputes live as you
   move. Hint becomes `"Set your End Position by moving the joystick on the
   screen and tapping the End button to save this spot."`
4. Joystick to the far corner, tap `End`.

If the user never picks points, the app synthesises a pair around the current
yaw giving exactly **180° horizontal, one row** (`:1408-1413`).

**Mechanical pitch limits** are enforced by dropping rows
(`getTakePicVerNum`, `:1609-1648`): with the Astro Kit fitted, **−34° to +51°**;
without it, **−67° to +51°** (`:47-50`).

### The shot-position grid

`PaShootingPreviewLayout` — a horizontally scrolling `hCount`-column grid of
vector cells. Cell 0 is labelled `"Start"`, cell N−1 `"End"`, the rest by
number, so **"1" and "N" never appear**. **No thumbnails and no per-cell
shot/pending/failed state** — a clear gap worth filling in your port.

Tapping a cell selects it; tapping the same cell again slews the head there
(`SP_SET_GIMBAL_POS` behind a `MovingGimbalDialog`). It does **not** take a
picture, despite the on-screen hint `"Tap again to go to the selected spot and
take a picture"`. The `"Take a shot"` button exists in the layout but is
permanently `gone`.

### Progress and cancellation

Remaining-shot countdown only, built by raw concatenation
(`PanoramaLayout.java:656-664`) — note the shipped missing space:
`"Remaining Shots:"` + N in non-English locales, `N + "Shots Remaining"` in
English. Initial value is `perSpot × hCount × vCount`, except 720 which is
`(hCount × vCount) + 2` — **the two extra frames are zenith/nadir and the grid
never shows them**.

`PANO` and Astro Pano are the **only modes with a pause control**
(`RightControlLayout.java:122-145`) → `SP_PANORAMA_PAUSE_SHOOT(bool)`. Stop
sends `SP_PANORAMIC_CANCLE()`, with a client-side watchdog that fakes a
cancellation if the head goes quiet (`MainActivity.java:2956-2971`).

### Presets

Three independent lists — PANO PRO, 720 and Astro Pano PRO each have their own.
Header `"My Saves"`, add `"Save Current Values"`, empty `"No Saves"`, recall via
a green `"Apply Preset"` overlay, delete by swipe-left → trash → `"Confirm"`.

**Presets are not nameable** — `ProPanoParamModel` has no name field and no
dialogue has a text input; rows are identified only by their rendered parameter
summary. There is no slot limit, no overwrite, and no dedup (the model has no
`equals()`, so saving twice creates two identical rows). Stored in SQLite via
LiteORM in `benor.db`. **Give your presets names; it is a one-line improvement
over the original.**

### What a naive panorama port gets wrong

1. **Mixed units on one command.** `gimbal:x,y,z` are **radians** as decimal
   strings; `para:…,hAngle,vAngle` are **degrees**. Both ride cmd 271
   (`PolarisOrderCommunication.java:1167-1173`, `PanoramaLayout.java:622,628`).
2. **The sweep remainder is signed.** `getTotalAngle` returns `f % 360.0f` and
   Java keeps the sign; −350° stays −350°. That sign is the *only* source of
   shooting direction. Normalising to `[0,360)` silently reverses the sweep.
3. **Every persisted picker value is an index, not a quantity.**
   `normalPanoLevelAngle = 1` means 0.1°, not 1°. Reordering any table
   re-points every saved preset at a different lens. There is no version tag.
4. **The pitch-limit sign mapping flips with the Astro Kit fitted**
   (`calculateMaxVerAngle`, `:1974-2050`), and `getTakePicVerNum` **does not
   apply that flip** — so normal-pano and pro-pano disagree about what direction
   1 means. Pick one convention and use it everywhere.
5. **The row clamp decrements by exactly one row, not in a loop**, so
   overshooting by more than one step leaves the count too high — and it can
   drive the vertical count to **0**.
6. **`proTakeDirection` and `normalPaDirection` are the same wire field with two
   different meanings** — sweep signs in PRO/720, start corner in normal.
7. **The FOV call swaps width and height**: `initParameter(100.0, focal,
   sensor.h, sensor.w)`, so `filmHeight` receives the *short* side; Portrait then
   swaps again. Both swaps must be reproduced. `NormFovHelper`'s own field
   defaults use the opposite convention and are dead decoys.
8. **The per-focal-length FOV values in the picker are full-frame only** and are
   for display; the real math always recomputes from the selected sensor. Using
   them directly breaks every crop-sensor user.
9. **Sensor `multiple` (crop factor) is dead data** — never read. Applying it
   double-counts.
10. **Per-spot count > 1 disables People Remover** (`MainActivity.java:1407-1416`).
11. **Interval changes are sent live, mid-shoot**
    (`SP_PANORAMA_INTERVAL_SHOOT`), while count only reaches the head in the
    start order.
12. **`"Return to Start Position"` polarity is inverted** — checked sends
    `state:0` (`InnerSettingDialog.java:474-484`).
13. **Three fully independent copies of every optics setting** exist
    (`…`, `…720`, `…Sky`). Changing focal length in PRO does not affect 720.
14. `SP_PANORAMIC_START` has **zero call sites** and `MainActivity.clickStart`
    failed to decompile, so the exact start payload is *inference*, not fact.
    Capture it from the wire before relying on it.
15. Dead resources not to port: `NorPanoTakePicStartPointSelectDialog`,
    `NormalPanoAnglePicCountSelectDialog`, `pa_camera_aperture_dialog_layout.xml`
    and the string `as_start_point` all have zero call sites.

## 3.2 ASTRO — celestial tracking and star fields

Mode ID 8, `StarrySkyLayout.java`. **Three sub-modes** on `skyModeSelectType`
(default 0): an interval sequence for star trails / tracked stacks, and the two
Astro Pano variants covered in §3.1.

This is the richest program in the app and the one most entangled with the
handset. Read §3.2.6 before planning a port.

### 3.2.1 Alignment — one star, not polar

The in-app procedure (`sky_star_help_content_a.xml`), verbatim:

1. `"Please first adjust the height of each tripod leg. Try to keep the center of
   gravity of the device in the vertical center of the three legs."`
2. `"And then adjust the device to get it as level as possible."`
3. `"Be sure to adjust the angle between the camera and the quick release plate
   to vertical."`
4. `"1. Calibrate your phone's compass"` — `"For accurate positioning, be sure to
   calibrate before searching for stars."`
5. `"2. Celestial Position Alignment"` — `"Select a star that is visible in the
   current sky and you know the name of as an alignment star."` → `"Go to [Object
   Lists], select Jupiter and click [GOTO]."` → the head slews and
   `"automatically enter[s] celestial alignment mode"` → `"Move the virtual
   joystick in the screen interface to move the position of Jupiter into the red
   circle in the center of the screen."` → `"Tap the [Confirm] button…"`
6. `"3. Start Tracking (Astro Kit is required to use this feature)"`

On the wire it is **one-star alignment**: `num` in `SP_CALIBRATE_START` is
hardcoded to `1` (`StarrySkyLayout.java:857-862`). The sequence is
`step:1` (enter align mode) → user centres the star with the joystick →
`step:2` with the recomputed target angles (`MainActivity.java:364-388`) →
success. `step:3` exits.

While aligning: the AR sky map is forced to alpha 0, a red reticle appears
(`AlignFrameView` in `activity_main.xml`), a magnifier is available, and the top
banner reads `"Use the virtual joystick to move the selected star inside the red
circle of the center of the screen, then click the Confirm button"`. Buttons are
`"Confirm"` and `"Exit"`.

**Tracking state machine** — `starchaserResult(String)`
(`StarrySkyLayout.java:722-760`), ASCII digits: `"0"` aligned/idle (show **Start
Tracking**), `"1"` tracking, `"2"` transitional, `"3"` **not aligned** — hide the
tracking bar and raise `"Celestial position is not yet aligned, please go to the
star search list first and select a star [ GOTO ], this star will be used as an
alignment star."`

Failure surfaces as the toast `"Failed to Find the Star"`, driven by a **3-second
watchdog** (`MainActivity.java:1500, 2710`) — any bridge slower than that
produces spurious failures.

### 3.2.2 Tracking rates — two independent axes

**Rate body** (`speedType`, the `speed:` field): `0` = `"Stellar Rotation"`,
`2` = `"Lunar Rotation"`. **Not user-selectable** — it is set implicitly by the
target (`StarrySkyUtils.java:718-720`): choosing the Moon silently switches to
lunar rate. Default 0.

**Rate multiplier** (`halfSpeed`, a separate command): `"Full-speed Star
Tracking"` / `"Half-speed Star Tracking"`, chosen in the **help dialogue's
"Basic Settings" tab**, not on the main screen (`StarHelpDialog.java:377-402`).
Rationale: `"You can switch the movement speed according to the star-tracking
needs."` Half rate is the landscape-preserving compromise — sky and ground each
trail half as much.

Starting tracking toasts the combination, e.g. `"Stellar Rotation - Full-speed
Tracking"`. Controls: `"Start Tracking"` → `"Tracking"` → tap → `"Stop
Tracking?"`.

**The tracking bar only appears when the Astro Kit is attached** *and* alignment
succeeded (`StarrySkyLayout.java:885-914`). Without the kit the head can GOTO and
align but cannot track.

### 3.2.3 Target selection

The catalog is a **Google Sky Map (Stardroid) fork** — `source.proto` still
carries `Copyright 2010 Google Inc.`, and the application class is
`StardroidApplication`. Assets: `stars.binary` (3,186 sources),
`constellations.binary` (95), `messier.binary` (114, m1–m110), and
`stars_a.sqlite3` (3.8 MB, 15,449 HYG rows). The runtime star layer is ~6,278
sources. **There is no magnitude field** — magnitude was baked into the render
point size. RA/Dec in the protobuf are **degrees**; in SQLite `ra` is **hours**.

The picker (`"Object Lists"`) is rebuilt **every 600 s** and ordered: Sun, Moon,
8 planets, **37 named stars**, then **89 constellations — but only after
alignment** (`StarrySkyUtils.java:511`). Before alignment the grid is much
shorter; a port that always shows everything changes the whole first-run flow.

Title switches from `"Object Lists - Visible Now"` to `"Object Lists - Visible
Today"` once aligned. Non-visible entries render at 0.6 alpha.

Rise/set for stars uses **XEphem via JNI** (`libnative-lib.so`), fed an `.edb`
line with a hardcoded magnitude 6.3 and epoch 2000; Sun/Moon/planets use the
Java Stardroid solver instead. **Two engines that disagree at the margins.**

**Manual RA/Dec entry** — read-only fields that open scroll-picker sheets:

| Field | Wheels | Ranges |
|---|---|---|
| RA | hour / minute / second | `0–23h`, `0–59m`, `0–59s` |
| Dec | sign / degree / minute / second | `Latitude(N)` / `Latitude(S)`, `0–90°`, `0–60'`, `0–60"` |

Dec minutes and seconds run **0–60 inclusive**, so `90°60'60"` is selectable —
an original off-by-one. RA converts to **degrees**: `h*15 + m*0.25 + s*0.0041667`
(`SearchStarDialog.java:141-147`). Manual targets get the sentinel name
`"user input"`.

**Search** is a prefix trie — no fuzzy matching, no ranking — and names resolve
through Android resources, so **the index is built in the device language only**.
Recent searches cap at 6, labelled `"Recent Searches"` with `"Clear"`.

**GOTO** (a hardcoded literal, not a string resource) slews the head. The
in-flight overlay shows two rows, `Aims` and `Current`, each `Level D°M′S″` /
`Pitch D°M′S″`, with `Current` fed live from the head's own attitude push.
Cancel is a back-arrow.

If the target is below the safety pitch, a modal appears first — title
`"Cautions"`, body:

> `"This star is currently not visible, and the pitch angle of the device will
> exceed the safety value during GOTO, so please keep an eye on your lens at all
> times to prevent damage due to lens collision."`

with `"Continue"` / `"Exit"`. **Continue proceeds anyway.** Reproduce this
warning; it protects the lens.

### 3.2.4 Capture parameters (sub-mode 0)

`StarrySkyParameterView.java:83-149`.

| Parameter | Values | Default |
|---|---|---|
| Interval | `1–35 s` step 1, then `40–100` step 5, then `110, 120`, then `3, 5, 7, 10, 15, 20, 30, 40, 50, 60 min` | **4 s** |
| Shot count | `1`, **`∞`**, `2–19`, `20–100` step 10, then a long sparse tail to `5000` | **1** |

`∞` is the **second** item in the list, not the last. `2500` appears twice (a
source bug). When count is 1 the interval picker hides.

There is **no ISO/aperture/shutter control in this panel** — exposure comes from
the shared camera strip (§2.3); bulb seconds arrive as a separate field.

Progress: `"Taken: %d shots, left:%d shots"`, or
`"Taken: %d shots, left:∞ shots"` when unbounded.

### 3.2.5 Head-side toggles, buried in the help dialogue

Three switches that belong on the main screen but are only reachable from the
help sheet's `"Basic Settings"` tab:

- **`"Tilt Compensation"`** — `"When Polaris is not horizontal, turn on the tilt
  compensation to compensate for the tilt angle. If it is off, be sure to level
  the device."`
- **`"Dithering"`** — `"Turn on dithering to effectively remove hot pixels and
  allow stacked backgrounds to look more even."`
- **`"Enable Restricted Angle"`** — see §4.3.

**Surface these properly in your port.** Tilt compensation in particular changes
tracking accuracy and is currently three taps deep behind a help button.

There is also an **AR sky overlay** over the live view, opacity 0–100 (default
50), whose seekbar auto-hides after 2 s. It is forced off during alignment and
forced on during recording.

### 3.2.6 What a naive ASTRO port gets wrong

1. **GOTO angles are absolute, not a delta.** The app pins the pointing frame to
   the literal quaternion `{0.7071068, 0, 0, 0.7071068}`, computes the target
   angles in that canonical frame, then restores the real quaternion
   (`MainActivity.java:370/386`, `DrawSkyTools.java:480-562`). Skip the
   neutralisation and you send a delta from wherever the head happens to point.
2. **The head's attitude push must be re-armed every 5 seconds.**
   `SP_SET_AHRS_STATE(1)` is a heartbeat (`MainActivity.java:2983-2988`); without
   it the quaternion stream stops and every later GOTO computes against a stale
   frame. Send `(0)` when leaving ASTRO/SUN.
3. **`SP_SET_TRACK_HALF_SPEED` is double-inverted** — the caller passes
   `!halfSpeed` and the method inverts again. **Half speed on the wire is
   `halfSpeed:1;`.**
4. **Lunar rate is implicit.** There is no button; selecting the Moon sets it.
5. **A 3-second silence is treated as GOTO failure.**
6. **The sky engine applies no precession, nutation, aberration or proper
   motion** — J2000 mean-equator drawn as-is, obliquity a hardcoded
   `0.40909263f`, and the `pmra`/`pmdec` columns are never read. Adding modern
   reduction makes your app disagree with the head's own alignment model by
   ~0.4°. **Bug-for-bug fidelity is the safer choice**, because the head was
   aligned using these same coordinates.
7. **Rise/set times are shifted by the raw `ZONE_OFFSET`, excluding DST** — an
   hour wrong around a DST boundary.
8. **Shot count is persisted as a list *position*, not a value.** Reordering the
   list silently corrupts saved preferences.
9. **Displayed angles are sign-flipped inconsistently** — `setLevelAndPitchText`
   uses `-pitch`, the sun dialogue flips again and the star dialogue does not,
   and `Aims` azimuth is mapped `d → 360 − d` while `Current` is not.
10. **You cannot change shooting mode while tracking** — toast `"Begin Star
    Tracking (Cannot Switch Shooting Modes)"`.
11. **Constellation centres are hardcoded sexagesimal literals** in Java, not
    catalog data.

## 3.3 SUN — sunrise/sunset scheduling

Mode ID 7, `SunLayout.java`, `res/layout/sunsetlayout.xml`. It is a
**sunrise/sunset time-lapse scheduler with a solar GOTO** — not a solar tracker,
and there is no eclipse support anywhere in the tree.

**Flow**: `[Sunrise hh:mm | Sunset hh:mm]` → tap → GOTO the sun's position *at
that future instant* → parameter screen → shutter → reservation countdown →
recording.

Rise/set come from the Stardroid Java solver with horizon `-0.83°`, computed for
the phone's location.

| Parameter | Label | Range | Default |
|---|---|---|---|
| Location | `"Current Location"` + `(GMT+n)` | decimal degrees, e.g. `60.849°N 30.95°E` | from GPS |
| Start | `"Start From"` | dragged on a timeline, 1-minute snap, **clamped to now ± 1 hour** | golden-hour edge |
| End | `"Ends At"` | same, minimum span **3 minutes** | |
| Interval | `"Time Interval"` | **USB mode**: `1, 2, 3, 4 s`; **Shutter mode**: `0.5, 1, 1.5 … 4.5 s`; both then `5–20` step 1, `23, 25, 27, 30`, `35–100` step 5, `110, 120`, then `3–60 min` | **1.0 s** |

Golden-hour windows are `±180 min` around the event, with the window extending
**60 min past sunrise** and **30 min past sunset** (`SunLayout.java:39-49`).

While the sun-path polyline is drawn over the video, a passive top bar warns:
`"Do not adjust the rotation angle significantly, as this may cause you to lose
the sun path line on the screen."`

**Reservation**: a countdown labelled `"Until Start"`, state `"Reservation
Made"`, cancel via `"Call Off"` → `"Yes, Cancel"`. Optionally writes **two phone
calendar events** — `"Reminder"` at the start and `"Shooting Complete"` at the
end. A 10 s timer silently drags the start time forward if it slips into the
past.

**Port notes.** The solar GOTO sends `track:0` — the head parks at the sun's
*future* position and does not follow it. The ±1 hour clamp and 3-minute minimum
are real constraints the head enforces; a free date-time picker will let users
build schedules the head rejects. The calendar write has no browser equivalent —
offer a downloadable `.ics` or a local notification instead.

## 3.4 TIMELAPSE

Mode ID 4, `StaticLapseLayout.java` + `DelayParameterView.java`. Three inputs,
three derived readouts, all in one 125 dp strip that slides up from the bottom
when you tap the title bar (the joystick hides while it is open). It is an
inline panel, not a dialogue — tap the live view to dismiss.

| Label | Values | Default |
|---|---|---|
| `"Time Interval"` | **USB mode**: `1–35 s` step 1. **Shutter mode**: `0.5, 1, 1.5 … 4.5 s` first. Both then `40–100 s` step 5, `110, 120 s`, then `3, 5, 7, 10, 15, 20, 30, 40, 50, 60 min` | **4 s** |
| `"Shots"` | **`∞`** first, then `10, 30, 50, 70, 100, 130 … 1000, 1300 … 5000` — 42 entries | **`∞`** |
| `"FPS"` | `24, 25, 30, 60` | **24** |

Interval is a discrete list, never a free number. Note the default is
**unlimited shots** — the run continues until stopped or the card fills.

### Duration is derived, and the derivation is optimistic

`DelayParameterView.java:228-256`, rendered side by side as
`00:30:00 >>> 00:10:00`:

```
Shooting Duration = (shots − 1) × interval     # the GAPS, not the frames
Video Duration    = ceil(shots / fps)
```

Both are replaced by an infinity glyph when shots is `∞`.

**Neither figure accounts for the shutter or the camera's processing time**, and
the app never corrects for it. Which of the two interval policies is in force
(§4.1) decides how wrong it is: under `"Option 1"` the real elapsed time is
always longer, and under `"Option 2"` it is longer whenever exposure plus
processing exceeds the interval. **Treat the displayed duration as a lower
bound, and compute a better one in your port** — this is the easiest real
improvement on the original.

**There is no validation at all** — no check that interval exceeds shutter, and
no start guard. And **there is no self-timer or delay-before-start** in any of
these modes; scheduled starts exist only for `FREE PROGRAM` and `SUN`.

### Feedback and cancellation

A centred pill 25 dp above the bottom edge, with correct singular/plural:
`"<n> Shots Taken, <n> Shots Remaining"`, or `"<n> Shots Taken, ∞ Shots
Remaining"`. The live view stays fully visible; only the parameter strip closes.

**The counter is a self-sustaining poll, not a stream** — the head replies with
the remaining count and the app immediately re-requests
(`PolarisOrderCommunication.java:2111-2129`). Drop that and the counter freezes.

Stop → `SP_DELAY_SHOT_CANCEL()`, with a watchdog.

## 3.5 PATH-LAPSE — motion time-lapse

Mode ID 5, `DynamicLapseLayout.java` (1,131 lines). The user records up to
**8 keyframes** by jogging the head and tapping `+`; the head then moves linearly
between them while shooting.

### Recording a point, and the interlock that teaches the gesture

Minimum **2 points** to arm the shutter; at **8** the `+` button is removed
outright (`:843-847`). Points appear as 52 dp live-view thumbnails in a
horizontal strip.

After the first point the `+` button is **greyed out and disabled**, and only a
joystick touch revives it (`:960-963`). Three coach strings cycle to explain:

| State | Copy |
|---|---|
| 0 points | `"Please click on the + to take two different spots for shooting"` |
| 1 point, joystick untouched | `"Sliding the joystick to pick the second shooting spot"` |
| 1 point, joystick moved | `"Once the second shot spot is determined, click + to take the spot"` |

That interlock is a genuinely good piece of design — it makes an unfamiliar
two-step gesture self-teaching without a tutorial. Copy it.

**A point stores pan, tilt and roll only** (`DynamiclapsePoint`: `x, y, z` =
yaw, pitch, roll in radians) plus a thumbnail. **No zoom, no focus, no per-point
exposure, and no easing curve.** Motion is explicitly `"Linear Movement"`.

### The rate model

> `"For the first time, please set the shooting interval and the number of shots
> carefully; Polaris will automatically calculate the movement rate (i.e., the
> number of shots per unit of displacement) based on the distance between the two
> shooting spots, and the subsequent linear movement will follow this rate."`

Mechanically (`:944-1035`): the angle between consecutive orientations is a
**quaternion great-circle angle**, not a yaw difference
(`GetTwoPoinntAngle.GetTheta`). On point 2 the app latches
`angleSpeed = shots / degrees` — labelled `"Shots/1° Rotation"` — and every
later leg's shot count is *computed* from it.

Guards: an angle under **0.5°** yields zero shots and toasts `"No change in
shooting points, please set the number of shots, or re-set the spots."`; a
computed count over **9999** clamps and toasts `"The actual number of shots has
exceeded the limit (≤ 9999 shots), please reset the number of shots / reset the
spot."`

Two editors override the automatic rate, both documented in the help sheet:
tapping a leg's count chip edits **that leg only**; tapping the title bar sets
interval, count and FPS as the new default rate for everything after it.

**PATH-LAPSE uses a different count widget from TIMELAPSE** — four odometer
digit wheels giving a free integer **1–9999** with **no `∞`**, default **10**.
Its interval default is also different: **6 s** (USB) at **30 fps**.

### Speed sanity check, rehearsal, and feedback

When a parameter panel closes the app computes `(angle / shots) / interval` and,
if it exceeds **1.0 °/s**, raises the dismissible banner `"Please set the
interval and shots properly so the Polaris can reach the next position within
the set time range."` **It is advisory only and does not block a start.**

**`"Preview"`** rehearses the path: it issues the same start command with a
preview flag, and shows a green `"Previewing the track"` chip with an X that
cancels. Visible once there are 2+ points. This is a strong feature and cheap to
port.

Feedback during a run is the same shots pill plus a **segmented progress bar**,
one segment per leg, widths bucketed by shot count.

A first-run help sheet (`"How to shoot Path-lapse?"`) appears once, with a
`"Basic Settings"` tab holding a single switch, `"Return to Start Position After
Shooting"` — **inverted on the wire: checked sends `0`**.

## 3.6 HDR

Mode ID 6, `HDRLayout.java`. **Its own mode, not a modifier** — it does not stack
with timelapse or panorama anywhere in this app. **Blocked in Shutter mode**
(`"Please switch to the USB Mode to enable this feature"`).

**The bracket is always exactly three frames.** There is no count picker and no
EV-step picker. Three tiles, each showing an aperture/shutter pair and two lines:

| Tile | Copy |
|---|---|
| Under | `"Under-exposure"` / `"Will preserve bright details in the image"` |
| Normal | `"Normal Exposure"` / `"Will preserve gray details in the image"` |
| Over | `"Over-exposure"` / `"Will preserve dark details in the image"` |

**It brackets the shutter, not exposure compensation.** Under is the normal
shutter ÷ 2, over is × 2 (clamped to 3600 s in the bulb case) — but the doubled
value is then **snapped to the camera's own shutter list**, nearest match if the
exact stop is absent. So the real step is "one stop, quantised to what the camera
offers". `evPosition` is transmitted but the bracket logic never touches it.

Those are seeds only: **tapping a tile opens the full camera panel and edits that
frame alone**, across all five axes (S, F, EV, ISO, WB). The selected tile is
green. There is also a `"Take a Shot"` single-frame test.

Feedback is a `"3 Shots Remaining"` pill counting down; the parameter strip and
right rail hide during the run. Stop sends `SP_HDR_CANCLE()`. On completion the
app re-reads the camera and re-highlights whichever tile it actually ended on.

**A bug not to reproduce:** if any of the three frames has incomplete
parameters, the start command is skipped but the UI still arms — the run appears
to start and nothing happens.

## 3.7 Holy Grail — day-to-night ramping

`grail_model` = `"Holy Grail"`. **Not a mode.** It is a toggle surfaced as an
icon inside `TIMELAPSE`, `PATH-LAPSE` and `ASTRO`, opening a large configuration
dialogue (`GrailModelDialog2.java`, 1,363 lines). **Hidden entirely in Shutter
mode.**

### Target Brightness — a 49 × 21 grid, in EV

`"Target Brightness"` / `"Adjust shooting time on horizontal axis, set target
brightness vertically."`

- **X axis: 49 vertical lines at 30-minute spacing = exactly 24 hours**, starting
  at the current half hour. Points snap to those lines and nowhere else.
- **Y axis: 21 rows, −5.0 to +5.0 EV in 0.5 EV steps.** Exact match required.
- Pinch to zoom horizontally; 12/24-hour display switch.
- Up to 49 points. **The point on line 0 is mandatory and cannot be deleted**; if
  missing it is auto-created at **0 EV**.
- Curve is green; the already-elapsed portion greys out, advancing on a
  one-minute timer while a run is in progress.

Controls: `"Edit"` ⇄ `"Complete"` gates Reset and Delete (both tint-disabled
until Edit is on). Reset is **the only modal in the whole Holy Grail flow** —
title `"Unable to revoke after resetting"`, body `"Reset the Target
Brightness"`, buttons `"Reset"` / `"Cancel"`. It clears the curve back to the
single 0 EV anchor.

A separate day↔night bias slider shapes the overall trend:
`"Please adjust the changing trend of brightness according to actual
requirements. Slide the cube to the left from day to night. Vice versa."`
*(That this slider is a single scalar bias rather than a set of named curves is
an inference from the backing field being one float; the code does not say so
outright.)*

### Priority — a drag-to-reorder list

`"Priority"` / `"Prioritize with a long press, set with a click."` Three rows —
**S** (shutter), **ISO**, **F** (aperture) — reordered by long-press drag to
decide which axis the ramp consumes first. **Default order is S → ISO → F.**
A click opens that row's range sliders. Both gestures on one control; the hint
string is doing a lot of work.

### The three ramp axes

Each row has an enable switch and a range slider, populated from the connected
camera's own lists.

| Axis | Handles | Notes |
|---|---|---|
| **Shutter** | **four** — safe-low, span-low, span-high, safe-high | `"Set the shutter time and safe shutter speed span. After setting, the value will be taken within the set span when shooting."` Labelled `"Safe Shutter Speed["` |
| **ISO** | two | `"Unlock to set the ISO value range…"` — **hard-capped at 6400** regardless of the camera, and `auto` entries are filtered out |
| **Aperture** | two | `"Unlock to set the F (aperture) span…"` |

The nested four-handle shutter slider — a working span inside a safety span — is
the most sophisticated control in the app and worth reproducing.

### Where the brightness measurement comes from

**An external accessory, not the images.** The dialogue carries a hardware
section for the **`"Optical Matrix Sensor Module"`**:

> `"Achieve dynamic exposure adjustment and smoothly ramp from daylight to night
> transitions via ambient light change and advanced algorithms."`

with connection state `"Connected"` / `"Detecting"` and its own firmware update
path. **The ramping algorithm runs on the Polaris firmware**; the app only
uploads a curve and three ranges, then reads back a runtime brightness value.
There is no histogram analysis and no image download in this path.

**This matters for the port**: without that accessory a web app cannot
replicate the ramp itself — at most it can configure a curve the head will
follow. Building your own metering from downloaded JPEG histograms is a
different (and arguably better) design, but it is *not* what this app does.

### Presets — the only named ones in the app

`"Save current parameter＋"`, listed under `"My Saves"`, empty state
`"No Saves"`. A new entry is created as `"Holy Grail 01"` and immediately opens
in rename mode with the hint `"(eg: From sunset to sunrise, World Park)"`.
Rename is inline with **no length or character restriction**; delete is a
two-tap reveal-then-`"Confirm"`; apply is `"Apply Preset"`. No cap on count.

**A preset stores curve points as line indices, not clock times**, so applying
one slides the whole shape to the current half hour rather than restoring its
original time of day. That is probably the right behaviour, but it is
surprising and should be stated in the UI.

### Operating preconditions, verbatim

The app states these and they are real — the ramp will not work otherwise:

> `"·Please set the JPG included image format. ·Turn off the Auto Review
> function in the camera setting. ·Please set the camera to M mode. · Do not set
> the AUTO mode in ISO. · Recommend to set the JPG images in a smaller quality
> size. This can improve shooting speed."`

JPEG must be in the format set so the head can decode a frame; Auto Review
blocks PTP; **M mode and non-auto ISO are required so the head owns S/F/ISO**;
smaller JPEGs shorten each transfer and therefore the achievable interval.


## 3.8 FOCUS STACK

Mode ID 3, `FocusTrackLayout.java`. **The class name is misleading — this is
focus bracketing, not subject tracking.** Every user-facing string says `"FOCUS
STACK"`, every command is `SP_FOCUS_STACK_*`, the workflow racks the lens between
two focus limits, and the output is stitched into `all_in_focus.jpg`. There is no
bounding box and no tracker anywhere in the mode. **Name it "Focus Stack" in your
port and never mention tracking.**

Unavailable in Shutter mode.

### Setting the near and far limits — the user racks focus by hand

1. Tap `"Set focus distance"` — the MF panel opens and all other chrome hides.
2. **Four press-and-hold focus buttons**, coarse and fine in each direction.
   First fire at 30 ms, then auto-repeat every **300 ms** while held
   (`FocusTrackLayout.java:591-603`), with a haptic per press. The steps are
   `−4` (macro, coarse), `−1` (macro, fine), `+1` (infinity, fine), `+4`
   (infinity, coarse).
3. Tap `"Set starting point"`.
4. The button relabels to `"Set ending point"`, and a
   `"Back to starting point"` affordance appears.
5. Rack to the other limit and tap again. **`"Preview"` becomes enabled**
   (alpha 0.5 → 1.0) and the shutter button appears.

**Direction is implicit in which end you mark first.** The head stores the
endpoints; the app keeps only a boolean saying whether they are set. **The
shutter button is hidden until both are marked** (`RightControlLayout.java:201`).

The head relays focus commands to the camera over its own tether — the app never
touches the camera directly. The camera must be in **MF** for any of this to
work, and the app's own tips say so: `"To perform MF Focus feature via the Theta
APP or to use the Focus Stack mode, please set the camera's focus mode to manual
(MF) mode in advance."` and `"After focus stack parameters has been set, please
don't adjust camera focus manually."`

### The only numeric parameter

`"Shots"`, one horizontal wheel of **67 entries** built at
`FocusTrackLayout.java:161-187`:

- `2 … 30` step 1
- `31 … 99` keeping only values ending in 3, 5, 7 or 0 → `33, 35, 37, 40, 43 … 97`
- `100 … 200` step 10

**Minimum 2, maximum 200, default index 12 → 14 shots.** (The `10` in the layout
is a design-time placeholder, not the default.)

### Preview, progress and cancellation

`"Preview"` runs a dry traverse: the UI switches to `"Previewing"` with an
`n / total` counter, cancellable. *(That the preview traverses without capturing
is an inference — it is offered before the shutter and reports only a step
counter; the semantics live in firmware.)*

During the real run, progress is a **remaining-shots countdown only** —
`"14 Shots Remaining"`, correctly singularised to `"1 Shot Remaining"`. Cancel
sends `SP_FOCUS_STACK_CANCLE()` with a 2 s watchdog. Reconnecting mid-run
restores either the capture or the preview state.

### Strings that look like they belong here but do not

Four strings mislead. **None of these are Focus Stack:**

- `focusstack_adjust_range` = `"Focus Distance"` — **dead**, referenced by no
  Java and no layout.
- `polaris_not_set_focus_stack_parameter` — likewise **completely
  unreferenced**.
- `pea_focus` = `"Peak Focus"` / `"Manual Focus Assist"` — the global
  **focus-peaking overlay** toggle in the settings sheet.
- `input_focus_other_value` = `"Enter Other Values >>>"` and `focus_setting` =
  `"Set"` — these belong to the **panorama lens focal-length picker**
  (`PaFoucsDialog`), whose title is `"Please match the focal length currently
  used by the camera"`. Do not put them on a focus-stack screen.

## 3.9 FREE PROGRAM — the keyframe timeline

Mode ID 9, `PrecompileLayout.java` + `view/curve/PrecompileCurveView.java`.
Internally called "precompile"; **the product name is `"FREE PROGRAM"`** and
"precompile" appears nowhere in the UI.

This is the most ambitious screen in the app and the one least like the others:
**a three-lane keyframe timeline**, closer to a DAW than to a waypoint list. The
user scrubs a horizontal time axis and drops keyframes onto three parallel lanes:

| Lane | A keyframe means |
|---|---|
| **Camera parameters** | at time *t*, set S / F / EV / ISO / WB to these values |
| **Photo** | at time *t*, take a picture — optionally starting an interval segment |
| **Angle** | at time *t*, the head is at this pan / tilt / roll |

The roll lane is **hidden unless the Astro Kit is fitted**.

### Building a program

- **Single-tap an empty spot on a lane to add a keyframe** there; tap an existing
  point to select it, tap it again to deselect. Lane hit-testing is by Y band.
- **Camera keyframe**: tapping S/F/EV/ISO/WB opens the shared picker; a tick
  commits.
- **Angle keyframe**: tapping Pan/Tilt/Roll enters jog mode — the head starts
  streaming its position every **200 ms**, you move it with the joystick, and the
  tick snapshots the current pose.
- **Interpolation mode: long-press the segment *between* two keyframes** for
  800 ms. Requires a real gap; adjacent points closer than the minimum distance
  are refused.
- **Delete**: select a point, tap the trash.
- **Jump to a time**: tap the time readout for H/M/S wheels.

**A program with zero photo keyframes cannot be started** — the shutter only
appears once at least one photo keyframe exists. There is no cap on keyframe
count.

### Parameters

| Parameter | Range | Default |
|---|---|---|
| Keyframe time | integer seconds, `0 … 215999` (picker is 3 × `0–59`) | 0 |
| Timeline zoom | `0.0022 … 2.0`, giving **2 s to 1800 s per grid cell**, 6 cells across | 1.0 (4 s/cell) |
| Photo interval | **0 = single shot**; otherwise `1 … (t₂ − t₁)` seconds | 0 |
| Pan / tilt / roll | stored as radian strings, displayed **−180 … +180 °** | 0 |

### The inversion that will bite

`isLineModel` reads like "smooth curve" and means **the opposite**:

| Flag | Label | Rendering | Wire |
|---|---|---|---|
| `isLineModel == true` | **`"Jump"`** | dashed connector | `mode:0` |
| `isLineModel == false` | **`"Smooth"`** | solid filled bar | `mode:1` |

**New keyframes default to `"Jump"`.** (The English is a poor rendering of
直接过渡, "direct transition".) Confirmed three ways — the highlight logic, the
click handlers, and the literal `"mode:" + (!isLineModel ? 1 : 0)`.

### Scheduling — the app's only real "appointment"

**A run can be booked for a future time, within the next 24 hours, at 5-minute
granularity.** The shutter for this mode is a bespoke two-way slider
(`PreRecordButtonView`): **slide up to start now, slide down to schedule**. The
top label is a hard-coded, non-localised `"Begin"`; the bottom is
`"Reservation"`.

The scheduler is a bezier-arc scrubber of **288 positions × 300 s = exactly 24
hours**, offering only `"Today"` and `"Tomorrow"` (each suffixed with the date).
Title `"Period"`, confirm `"Confirm"`. For today the arc starts at the next
5-minute slot and re-syncs every 60 s.

**The head keeps the schedule, not the phone** — there is no `AlarmManager` use
for this anywhere, so the phone can disconnect and the run still fires. That is
exactly the behaviour a device-hosted web app wants, and it already works.

The waiting panel shows `"Reservation Made"`, `"Time"`, `"Current Location"`
(rendered `60.85°N 30.95°E`) and `"Until Start"` counting down at 1 Hz — though
**the countdown clamps at 60 s and never shows less than `00:01`**. Cancelling is
two-tap: `"Call Off"` → `"Yes, Cancel"`, reverting if you touch elsewhere.

### Calendar reminders — and two real hazards

`"Add Reminder"` ⇄ `"Reminder Created"` writes **two events** to the phone's
calendar: `"Reminder"` at the start and `"Shooting Complete"` at the end, each
5 minutes long with an alarm.

Two things not to reproduce:

1. If no calendar account exists it **creates one named `boohee` /
   `BOOHEE@boohee.com`** — copy-pasted from a well-known Chinese snippet and
   nothing to do with Benro.
2. **Deletion matches on title only**, so cancelling a reminder deletes *every*
   event in the user's calendar titled `"Reminder"` or `"Shooting Complete"`.

A server-side scheduler on the device is both safer and better here.

### Progress, cancellation, and the missing feature

The running screen shows **a single countdown and nothing else** — no
per-keyframe progress, no shot counter, no current-position readout. Total time
is just the latest keyframe across the three lanes. *(A wrapping bug: the
countdown formatter computes hours as `(seconds/3600) % 24`, so a program longer
than a day displays a wrapped time.)*

Cancel sends `SP_PRECOMPILE_SHOT_CANCLE()` and also deletes the calendar
reminders.

**There is no save or recall.** `model/PrecompileSaveModel.java` declares a full
database table for exactly this and is **referenced nowhere in the APK** — never
constructed, never saved, never loaded. Worse, the app **discards the whole
program on `resetView()`**, which fires whenever the mode returns to its initial
state. The only persistence is crash recovery driven by the head.

**Adding save/load to your port is adding a feature, not porting one — and it is
probably the single highest-value addition available.** A timeline this
expressive that cannot be saved is close to unusable.

### Rendering spec, if you want it to look native

Background `#191A19`, grid `#24282D`, grid labels white at 34 %, playhead
`#B8CDE5`. Lane colours — camera `#3CAF63`, photo `#DBBC4A`, angle `#3CA2BC`,
each with a brighter selected variant and a dark segment fill. Dash pattern
`[10,10]`. Grid labels format as `MM'SS"`, `HH:MM'SS"` past an hour, and
`D#HH:MM'SS"` past a day. A 15 ms haptic fires on every point interaction.
## 3.10 Burst and bulb

**Burst** (`"Taken"`) is **PHOTO-mode only**. Two digit wheels, effective range
**2–99**, default **2** (re-forced to 2 on every launch). Gated three ways: the
camera manufacturer must be **Canon, Nikon, Sony or Panasonic**; control mode
must be USB; and it is disabled while People Remover or bulb is active. **The
count wheel is shown only for Nikon** — other brands get the toggle and the
camera's own drive setting governs the count. Warnings: `"Make sure the camera
has enabled Burst mode, otherwise it will fail to shoot."` and `"Polaris does
not save the images/videos taken in this mode, please view them through the
camera"`.

**Bulb** is not a separate control — the picker appears when the selected
shutter value is Bulb. Two wheels, minutes `0–59` and seconds `0–59`, **range
1 s to 59:59**, step 1 s, default **1 s**; `00:00` auto-corrects to `00:01`. The
camera must report bulb support, and **Canon + bulb suppresses live preview**
*(inference — the code sets a capability flag false without naming it)*.

In Shutter mode there is no camera shutter list at all, so the user declares the
exposure instead: minutes `0–99`, seconds `0–59`, milliseconds `0–900` in 100 ms
steps, with the warning `"Please adjust the camera shooting parameters on the
camera. The time set here must not be lower than the true exposure time in the
camera settings."`

## 3.11 Time-lapse family: what a naive port gets wrong

1. **`(shots − 1) × interval`, not `shots × interval`.** Off by one full
   interval.
2. **The duration readout ignores shutter and processing time entirely**, and
   which of the two interval policies is active changes the answer.
3. **PATH-LAPSE hardcodes `bulb:0`** (`DynamicLapseLayout.java:630`) while
   TIMELAPSE sends the real value. Long-exposure path-lapse silently loses the
   bulb duration — a real bug.
4. **Per-leg interval and count belong to the *outgoing* leg; the last point's
   values are ignored.**
5. **The two modes use different count widgets with different ranges and
   defaults** — 42 presets including `∞` at 4 s / 24 fps for TIMELAPSE, versus
   free digits 1–9999 with no `∞` at 6 s / 30 fps for PATH-LAPSE.
6. **`angleSpeed` is latched from the first pair only.** Editing the first leg's
   count after adding a third point recomputes nothing downstream.
7. **Angles are quaternion great-circle, not Δyaw**, and the Astro Kit changes
   the math (an extra 45° Y rotation composed with roll). A Euclidean yaw
   difference gives wrong shot counts on any tilted move.
8. **Gimbal coordinates arrive in radians** while the 0.5° and 1.0 °/s
   thresholds are in degrees.
9. **`SP_DELAY_SHOT_SEND_END` contains a literal double semicolon** —
   `…;photoCnt:N;;preview:P;`. Reproduce it verbatim.
10. **HDR brackets the shutter, snapped to the camera's list — never EV.**
11. **HDR with incomplete parameters arms the UI but sends nothing.**
12. **Holy Grail commits only on dialogue dismiss** — nine separate commands fire
    at once when the sheet closes. Closing *is* saving. Surprising, and worth
    changing.
13. **Holy Grail brightness needs the Optical Matrix Sensor Module.**
14. **Curve points snap to 30-minute lines and 0.5 EV rows**, and the line-0
    point cannot be deleted.
15. **Holy Grail ISO is capped at 6400** regardless of what the camera supports.
16. **The remaining-count readout is a poll the app must keep re-arming**, not a
    push.
17. **PATH-LAPSE's saved state loses the gimbal coordinates** — the persisted
    model declares a `point` field that is never written, so reconnecting
    mid-run restores the schedule but not the path or the thumbnails.
18. **Control mode gating is asymmetric**: Shutter mode blocks HDR, VIDEO, FOCUS
    STACK and FREE PROGRAM, hides the Holy Grail icon, hides the TIMELAPSE
    counter on recovery — and *adds* sub-second intervals that USB mode does not
    offer.

---

# 4. Settings and device management

Settings live in three places: an **on-screen sheet** over the live view, the
shell's **Settings fragment**, and a scattering of head-side toggles buried in
help dialogues.

> **Two complete settings UIs ship in the binary.** `SettingFragment`
> (`/fragment/setting`, the current one) and `SettingActivity` (1,383 lines,
> legacy, reachable only via a hidden long-press). Port the fragment.

## 4.1 The on-screen settings sheet

`res/layout/dialog_inner_setting.xml` / `polaris/dialog/InnerSettingDialog.java`.
Opened from the right rail. Two tabs: **`"Overall Settings"`** and **`"Shooting
Mode Settings"`**. Every row is a control plus a full explanatory sentence — the
app explains *why*, not just *what*, and that is much of why it feels
trustworthy.

### `"Overall Settings"`

| Control | Default | Where it lives | Explanatory copy (verbatim) |
|---|---|---|---|
| `"Floating Thumbnail"` | on (local) / off (remote) | app | `"A thumbnail image will appear in the bottom left corner of the preview screen after each image is taken"` |
| `"On-screen HIST"` | off | app | `"The preview screen will display the histogram in real time"` |
| `"Peak Focus"` | off | app | `"Manual Focus Assist"` |
| `"Grid"` | off | app | `"Grid display for composition"` |
| `"Auto Level"` | device | **head** | `"Turn it on, tap the icon (…) on the camera interface to trigger Polaris auto level adjustment. It can not be accessed in dynamic shooting modes, such as PATH-LAPSE, in which manually tripod and Polaris level adjustments are required."` |
| `"L-Bracket Lens Correction"` | device | **head** | `"Once turned on Polaris will automatically correct the camera lens direction (for celestial tracking only)"` |
| `"Live Preview Orientation Switch"` | off; **Counter Clockwise 90°** preselected | app | `"You can turn on this feature if the camera is in vertical shooting…"` then `"Clockwise 90°"` / `"Counter Clockwise 90°"` |
| `"Auto Stitching"` | off | app | `"With this function turned on, stitchable images (such as those taken in panorama, HDR, etc.) can be automatically stitched by going to the media library and clicking on their image groups."` |
| `"GPS Info"` | — | app | row → manual coordinate entry |

`"L-Bracket Lens Correction"` gets a full-screen explainer before it can be
enabled, ending in the attestation button `"Confirmed, Turn It On"`.

### `"Shooting Mode Settings"`

**`"Return to Start Position After Shooting"`** — two independent switches,
`"PANO"` and `"PATH-LAPSE"`. **Polarity is inverted on the wire: checked sends
`state:0`** (`InnerSettingDialog.java:474-484`).

**`"Time-lapse Interval"`** — a two-option radio group whose labels *are* the
formulas, and the sharpest writing in the app. **Default is Option 2.**

- `"Option 1"` — `"Calculated by: actual shooting interval = set interval +
  shutter time + image processing time"`
- `"Option 2"` —
  - `"1. When the set interval ≥ shutter time + image processing time"` →
    `"Equation: actual shooting interval = set interval"`
  - `"2. When set interval﹤ shutter time+ image processing time"` →
    `"Equation: actual shooting interval = shutter time + image processing time"`

Option 1 makes the interval *additive* (a gap between frames); Option 2 makes it
*absolute* (a period, clamped upward when the exposure will not fit). **Port
both.** Getting this wrong silently changes the length of every time-lapse.

**`"Shutter Response Time"`** — a slider reading `"0s"`.
`InnerSettingDialog.java:320-337` maps the 0–100 slider position to **0–20
integer seconds**, clamped at 20; default **0 s** (`:339`). Sent **only on
touch-up**. Its explanation:

> `"Please set the shutter response time according to the camera and lens weight
> you are use, in order to solve the condition that the shutter has been
> triggered before the inertial oscillation that occurs when Polaris rotation is
> stopped, resulting in a blurred or trailing PANO shot. (Used for PANO Mode
> only, including Astro Pano)."`

Settle time between a slew and a release. Any panorama on a long lens needs it;
a default of 0 is poor. Consider 1–2 s.

## 4.2 Shell settings

Sections `"Devices Settings"` and `"Benro Connect"`. Disconnected, rows read
`"Device Not Connected"` and tapping toasts `"Please make a connection with the
Polaris device first"`.

| Label | Type | Options / default |
|---|---|---|
| `"Auto OFF"` | sub-screen | master toggle + `"After 10mins"` / `"After 30mins"`. **No "Never".** Default **10 min** |
| `"Format SD Card"` | confirm | `"Confirm"` / `"Cancel"` → `"Formatting..."` → `"Formatted Successfully"` / `"Formatting Failed"`, 10 s watchdog |
| `"Firmware Upgrade"` | panel | §4.5 |
| `"My Device"` | sub-screen | device info; **shows the device password in plaintext behind an eye toggle** |
| `"APN"` | sub-screen | APN / `"User Name"` / `"Password"`, auth `"None"` / `"PAP"` / `"CHAP"` / `"PAP/CHAP"`, default None |
| `"WiFi Range"` | sub-screen | `"2.4G"` / `"5G"` |
| `"USB Compatible Mode"` | toggle | `"With this feature turned on, more cameras will be compatible with Polaris through USB connection."` |
| `"Sound"` | toggle | warning tone |
| `"About Benro Connect"`, `"Clear Cache"`, `"Connection Help Guide"` | rows | |

`"WiFi Range"` hint: `"2.4G transfers farther than 5G, it is recommended for
remote distance. 5G transfers faster than 2.4G, it is recommended for a smoother
experience."` **Changing the band drops the connection** — the head re-brings-up
its AP. Plan the reconnect.

Auto-shutdown timeout of 10 minutes is worth surfacing prominently in a
device-hosted UI: a browser tab left open is not "use", and the head will switch
itself off mid-session.

**Absent from the entire tree**: screen/LED brightness, an in-app language
picker (locale is system-driven), metric/imperial, motor speed or acceleration
limits, and any Wi-Fi SSID / passphrase / channel / country setting. The only
unit toggle anywhere is GPS `D.D` ↔ `D.M.S`.

## 4.3 Safety limits

**`"Enable Restricted Angle"`** — off by default; turned on only to shoot the
zenith or below the horizon:

> `"You can open the Polaris rotation angle restrictions manually if you need to
> do the Zenith or Ground Angle Shooting."`

Enabling requires physically aligning the Astro Kit and confirming with
**`"Aligned Manually. Enable It"`**. Both extremes are warned:

- `"Zenith Shooting: Please pay attention to the distance between the Astro Kit
  and the Pitch axis of the Polaris in real time to avoid the collision."`
- `"Ground Angle Shooting: Please pay attention to the distance between the Astro
  Kit and the tripod in real time to avoid any collision during Polaris
  roration."`

With it enabled a `"Safety Tip"` dialogue restates the Roll/Pan motor collision
risk. The head reports its own hard stop as `"Polaris has reached its rotational
limit"`.

## 4.4 Levelling and calibration — two different things

**Auto Level** is device-side and ports cleanly. A full-screen overlay with an
animated level line, `"Leveling...."`, a success tick and `"Exit"`; on first use
a green coach-mark points at the status-bar icon. It **requires the Astro Kit**
and is refused in dynamic modes (`"Auto Level can not be accessed in this
feature."`).

**Calibration does not calibrate the head — it calibrates the *phone's*
compass** and uploads the resulting heading. Four states in one dialogue:

1. **Setup** — a diagram, a live heading readout (`"0°"`), and `"Please hold the
   phone parallel and close to the Polaris in the direction shown, and then click
   Calibrate."` Button `"Start Calibration"`.
2. **Figure-eight** — `"Recalibrate the phone's compass by waving the phone in
   the manner shown"`. The gate is real: it needs `|accel.x| ≥ 12`,
   `|accel.y| ≥ 12`, `accel.z ≥ 12` *and* `accel.z ≤ −5`, each observed more than
   30 times, plus magnetometer accuracy ≥ 1 (`CalibrationDialog.java:212-238`).
3. **Running** — `"Calibrating"`, 10 s timeout.
4. **Result** — `"Calibrated Successfully"` / `"Calibration Failed"`, `"Confirm"`.

It ends by sending the phone's true-north heading plus the phone's lat/long to
the head. **This is the biggest structural obstacle to a browser-hosted port**
and is discussed in §6.

## 4.5 Firmware update

Firmware comes from **Benro's cloud**, not the head:
`POST https://service.benro.com/benro/user/auth.do?cmd=queryromversion` with a
`product_id`, signed `MD5("SNOPPAANDROID@#101" + localTimestamp)`. Product ids:
app `110301000000`, head `110301200000`, **Astro Kit** `110301200100`.

**The phone downloads over WAN, then re-uploads to the head over Wi-Fi**, shown
as one 0–100 bar where 0→50 is the download and 50→100 the upload.
Upload is `PUT http://192.168.0.1/dav/<name>` expecting **HTTP 201**, with a
legacy `POST /sd` multipart fallback.

Preconditions, all enforced: battery **≥ 30 %**, SD present and healthy, **≥ 200
MB** free, WAN reachable, and **not** in cellular mode (`"Unable to upgrade in
Cellular Network Connection"`).

Copy: `"Upgrading"` *n*% → `"Upgrade Successful"` / `"Failed to Upgrade"`, with
the do-not-interrupt warning `"Do Not Turn of the Device / While the WLAN Blue
Light is Blinking / The Device Will Automatically Reboot When Upgrade is
Complete"` *(typo in source)*. A `"Back to Home"` escape appears after **5
minutes**.

**The "extra device" is the Astro Kit, not the camera** — confirmed by the
strings (`"Astro Kit Firmware is Up to Date."`, `"Upgrading the firmware of
Astro Kit"`), by `ExtraDevVersionInfo.getExAxisVersion()`, and by the
`hasTriaxial` gating. A third accessory, the `"Optical Matrix Sensor Module"`,
is the only one that streams real progress.

## 4.6 Control mode — USB vs Shutter

`res/layout/dialog_switch_take_model.xml`. Titled `"Switch Shooting Mode"`,
completed with `"Complete"`.

| Option | Copy |
|---|---|
| `"USB Mode"` | `"USB Mode is turned on by default."` |
| `"Shutter Mode"` | `"In Shutter Mode, only shutter can be controlled; No live view or parameters adjustment is supported. Some shooing modes, such as HDR, FOCUS STACK, etc. can not be accessed in Shutter mode."` |
| `"HDMI Live View"` | `"Turn on HDMI, successfully connect HDMI cables to both HDMI ports on Polaris and camera, Live view is available in the app."` |

**Switching requires a device reboot.** `SP_REBOOT_CONFIRM_MODE(true/false)`
(`SwitchTakeModelDialog.java:254, 266`), `"Please reboot Polaris"` /
`"Go to reboot Polaris"`, `"After rebooting, it will automatically switched to
USB Mode."`, and `MainActivity` closes the control screen
(`MainActivity.java:995-1002`). Not a live toggle.

Separately, a `"USB Mode"` / `"Compatibility Mode"` dialogue exists for camera
compatibility: `"If your camera cannot connect to Polaris successfully, please
try to turn Compatibility Mode on and then restart Polaris to connect again."`

## 4.7 Connection, discovery and the security model

The phone finds the head by **BLE scan** — there is no Wi-Fi scan. It filters
advertised names on the prefix `polaris_`, then derives the Wi-Fi **BSSID from
the BLE MAC by incrementing the last nibble** (`F`→`0` wraparound,
`PhoneConnectUtils.java:102-109`) and pins the join to it.

**A BLE GATT connect is used purely as a wake-up.** The instant the GATT
connection reaches `STATE_CONNECTED` the app closes it and joins the Wi-Fi
(`WifiBroadcast.java:1150-1190`). The head appears to keep its AP powered down
until a BLE connection wakes it.

**The head's access point is open.** `allowedKeyManagement.set(0)` =
`KeyMgmt.NONE` (`PhoneConnectUtils.java:60`), and the API-29 path builds a
`WifiNetworkSpecifier` with no passphrase at all. Phone takes a static
`192.168.0.{10..249}/24`; head is `192.168.0.1`.

**The device password is decorative.** It is an app-layer PIN over one command:

- `step:1` — **GET**: the head returns the password *and* the security answer to
  any client that asks.
- `step:2` — SET. `step:5` — RESET.
- "Encryption" is **plain Base64** (`UtilFunction.java:386-409`).
- Every check is **client-side** (`ChangePasswordAcitivty.java:382`,
  `ForgetPasswordAcitivty.java:360`).
- `"0000"` is a **sentinel meaning "unset"**, not a password.
- The label says `"Password Must Consist of 4-6 digits, letters, or symbols"`
  but the code accepts **3–6** characters.
- First-run is **skippable**, and a long-press backdoor in the legacy connect
  fragment resets the password and enters a tourist mode.

Anyone in Wi-Fi range can join the open AP, ask for `step:1`, and read the
password. **If your web app is reachable beyond the head's own AP, add real
authentication** — do not port this model faithfully.

There is **no cloud account anywhere**. `BenroAccount` is a local SQLite table.
The cellular "remote control" path uses an FRP reverse proxy plus an MQTT wake,
keyed only on the device id — which is the visible suffix of the broadcast name.

Six fixed security questions, sent as an **index 1–6**, answer stored Base64 on
the head and verified client-side, so recovery works fully offline.

**Facebook and Firebase Crashlytics are bundled but never called** — every
apparent import in the app's own code is jadx constant misattribution. **Do not
port an analytics layer.** The privacy-consent dialogue is likewise **dead
code**: zero instantiations, both buttons just dismiss, and the text is
hardcoded Chinese.

## 4.8 Status, telemetry and error codes

Head battery (`capacity` + `charge`), SD free space (**MB**, rendered
`"512MB"` / `" 12.3 GB"` — note the leading space in the GB branch, and `"None"`
when absent), the **Optical Matrix Sensor Module** battery (not the Astro Kit,
which reports no battery), camera identity, and Wi-Fi signal — the last read
from the **phone's** RSSI, not the head's.

**Low battery**, with charging suppressing both, and each firing **once per
session** (`MainActivity.java:2145-2172`):

| Range | Behaviour |
|---|---|
| ≥ 21 % | nothing |
| **17–20 %** | non-modal banner: `"The power of the device is less than 20%, please charge the device in time."` |
| **≤ 16 %** | modal, **uncancellable**: `"Device power is seriously low"` / `"Please charge the device in time to prevent the device from suddenly shutting down."` / `"Continue"` |
| < 30 % | firmware update blocked |

**No temperature telemetry exists**, and there is **no shots-remaining
estimate** — only free megabytes. Both are obvious additions for a better
client.

Device error codes worth surfacing verbatim (`polaris/utils/CameraErrorCode.java`):
`-1002` `"No camera detected. Please connect or turn on camera."`,
`-1003` `"Shot Timeout"`, `-1004` `"Please check camera connection."`,
`-1005` `"Camera is Busy"`, `-1006` `"SD Card Space is Insufficient"`,
`-1101` `"Abnormal camera parameters. Please restart camera."`,
`-1102` `"Be sure to select JPEG or JPEG+RAW…"`,
`-1201` `"Device Timeout. Please check for external motor interference."`,
`-1202` `"Astro Kit Not Detected."`,
`-1203` `"Polaris has reached its rotational limit"`,
`-2009` `"Continuous Shooting Is Not Supported"`.

## 4.9 Onboarding and help

The three-page tutorial is **not first-launch gated** — it is reached manually
from Discover → `"Operation Instructions"`. Page 2 is the only place the app
tells you how to switch the head on: `"Polaris Power On: Short press, then long
press the power botton and release till three beeps. Green power and Blue Wlan
indicators will be lit up."`

`"Connection Help Guide"` deep-links into Android system settings and carries
`"Wi-Fi Easily Disconnected?"` troubleshooting. The `"Discover"` tab ships
**placeholder Chinese lorem text and fake dates** — unfinished, do not model
anything on it.

## 4.10 The media library

Four landscape activities: `PhotoAlbumActivity` (the grid), `MediaActivity` (the
swipe viewer), `MediaExtraActivity` (the frames inside one composite capture),
and `MediaExtraItemActivity` (the viewer within a group).

### Where the files are

Two locations only, and **the camera's own card is never browsed**:

- **the head's SD card**, tab `"SD"` — served over plain HTTP from
  `http://192.168.0.1/sd/<mode>/[class_N/]<file>`, with WebDAV `PUT` to `/dav/…`
  for writes.
- **the phone**, one fixed folder: `<Download>/Polaris/`.

Mode directories are `normal`, `Lapse`, `focusStack`, `panorama`, `sun`, `HDR`,
`starSkyStack`. The head **pushes an unsolicited notification every time a file
is written** (`SP_ADD_FILE`), so the grid updates live. A poll-only port will
miss new captures — bridge that push.

Listing is paged **10 at a time** with a 1 s re-drive watchdog. Records carry
type, class, size in bytes, creation time as `yyyy-MM-dd HH:mm:ss` **with no
timezone**, duration in milliseconds, and path.

### There is no thumbnail endpoint

**This is the most important fact about the gallery.** For JPEG the "thumbnail"
URL *is* the full-resolution original; for RAW it points at the sidecar JPEG.
There is no `?size=`, no `/thumb`. **Every grid tile is a multi-megabyte
download.**

The app compensates by **serialising to one in-flight request** with a 3 s
watchdog. Do not fan out parallel `<img>` requests at the head — it will not cope.
Note also that the URL carries a cache-buster (`?<lastModified>`); reproduce it
or you will serve stale bytes after re-shooting to the same path.

**RAW never renders** — anything without `.jpg` in the URL falls back to a
placeholder glyph, in both the grid and the viewer.

Generating real thumbnails on the device is the single biggest improvement a
device-hosted app can make over the phone app, and it is easy: you are already
running next to the files.

### Browse model

**Six columns**, 5 dp gaps — the layout's `numColumns="5"` is dead, every runtime
path forces 6. **Except one column when the PANO filter is active.** Sticky date
headers group by local calendar day, labelled `"Aug 27, 2026"` in English. Sort
is newest-first by modification time. Empty state `"No Files"`.

A filter wheel runs down the right edge: `"All"`, `"PHOTO"`, `"PANO"`, `"FOCUS
STACK"`, `"TIMELAPSE"`, `"SUN"`, `"ASTRO"` — HDR is absent from the wheel and
handled as a bare literal elsewhere.

**Multi-select is a button, not a long-press.** `"Select"` becomes
`"Cancel (%1$s)"`. **Long-press opens a zoomed peek overlay instead** and
consumes the event. Getting this backwards changes the entire interaction model.
The action rail (select-all, download, delete) slides in over 300 ms; download is
hidden on the phone tab; disabled controls are conveyed by **alpha, not by being
disabled** — so they still swallow taps.

### The viewer

Title shows **position, not a date**: `"3/57"`. Chrome auto-hides after 5 s.
Photos use a pinch-zoom view; **EXIF rotation is applied only to phone-local
files**, so remote images are assumed upright. Video plays through ijkplayer
(ffmpeg) straight off the head — **a browser `<video>` element may not decode the
head's MOV codec at all; verify before assuming parity.**

The details panel is much smaller than its layout suggests: only `"File Name:"`,
`"Size:"`, `"Created:"`, plus a 256-bin luminance histogram and ISO / exposure /
f-number for photos. Resolution, format, focus, fps and duration rows all exist
in the layout, are populated by the code, and are **`visibility="gone"`**. Do not
"restore" them and call it parity.

Opening the details panel on a remote photo **downloads the full-resolution JPEG
twice more** — once for EXIF, once for the histogram — on top of the copy already
fetched for display. There is no server-side metadata or histogram endpoint.
There easily could be in yours.

### Download

**Concurrency is hard-wired to one.** A FIFO queue drains serially; a batch
selection flattens groups and enqueues everything. Transfer is HTTP with a
`RANGE` header and an appending write, so it is **resumable**, in 16 KB chunks.
Pre-flight refuses unless **1 GiB would remain free** afterwards.

**There is no retry.** An error drops the file and moves to the next one; the
only control is cancel. **There is no foreground service or notification** —
downloads run in-process and die with the screen.

Progress is in bytes, throttled to 50 ms, with a batch counter. *(Two bugs worth
reproducing as behaviour but not as code: the per-file percentage uses integer
division so it reads 0 % until it snaps to 100 %, and the progress bar casts
sizes to `int`, so it is wrong above 2 GiB.)*

Files land in `<Download>/Polaris/` with a `yyyyMMddHHmm` filename prefix.

Choice sheet: `"Download JPEG to Local"`, `"Download RAW to Local"`,
`"Download JPEG + RAW to Local"`, `"Cancel"`. Batch confirmation: `"If the
selected media file contains multiple photos, all the files within will be
downloaded. Total (%1$d) Pics"`.

### Delete

Simple sheet: `"Delete"` / `"Cancel"`, **no body text**. Composite sheet adds
`"If the selected media file contains multiple photos, all the files within will
be deleted, Total (%1$d) Pics"`.

Three things to know:

- **Phone-local deletes get no confirmation at all.**
- **Individual frames inside a focus stack, panorama, HDR or astro stack cannot
  be deleted** — only timelapse and sun groups allow per-frame deletion.
- **The delete response parser is an empty loop** — there is **no success or
  failure feedback**, the UI simply reloads. And there is no trash, no undo and
  no tombstone anywhere. **Add a confirmation and a result check in your port**;
  this is the one place the original is genuinely unsafe.

### Cellular warnings and preview quality

`"Cellular network is being used"` / `"Downloading to local may use large
amounts of data."` — but **it keys on the connection mode, not on the phone's
radio**; `ConnectivityManager` is never consulted. Porting this to
`navigator.connection` reproduces the wrong behaviour. There is also no
"don't ask again" for this particular sheet, though four other dialogues do
persist a `"Do Not Show Again"`.

**`change_remote_image_quality_dialog_layout.xml` has nothing to do with
downloads** — it tunes the **live MJPEG bitrate** and only appears in cellular
mode: `"Slide left and right to adjust the preview image quality"` / `"Lower
image quality for smoother preview"`. A continuous 0–100 slider, default 50,
mapped to `?action=signal&<v*40/100+10>` — so **0–100 maps to 10–50** — and
rate-limited to one request per second.

### Stitching — and why it belongs on the device

`PolarisSyntheticHelper` loads `libpanorama.so` (11 MB) plus OpenCV 4.5.3 and
**stitches in the phone**, not on the head and not on a server.

**The crucial architectural detail: the head hands the stitcher a solved camera
model.** Before stitching, the app fetches `ISP_stitching_config.yaml` (or the
focus-stack / HDR variant) containing OpenCV `detail::CameraParams` —
focal, principal point, rotation and translation — **derived from the gimbal's
own yaw and pitch**. The native code takes an `init_from_cameras` path rather
than feature-matching, which is why it is fast and why it succeeds on
low-texture skies.

| Capture | Output |
|---|---|
| Panorama | direct, spherical projection |
| Focus stack | `all_in_focus.jpg` |
| HDR | `hdr_ldr_final.jpg` |
| Astro stack | direct |

The result is written as `SP_out.jpg` and **uploaded back to the head**.

Limitations worth knowing:

- **Projection is hard-wired to `"spherical"`** and the output is a flat JPEG
  trimmed to the stitched region — **not a full equirect** unless the sweep
  covered one.
- **No EXIF and no XMP `GPano:*` is written**, so **no sphere viewer will
  recognise the output**. Writing that metadata is a trivial, high-value
  addition.
- **RAW frames are excluded** from stitching.
- **There is no progress.** `"Being Synthesized %1$d%%"` renders permanently as
  0 % because the setter is never called; the real UI is an indeterminate spinner
  reading `"Being Processed, Please Wait"`.
- **Cancellation is not real** — leaving the screen discards the result but does
  not stop the work.
- **The upload-back is hard-coded to `192.168.0.1`** while the frame download
  follows the configured resource address, so remote mode can stitch but cannot
  write the result home.
- Stitching is triggered automatically when you open a group, gated on
  `"Auto Stitching"`, which **defaults to off**.

Result copy: `"Synthesis success"` and, memorably, `"Synthesis is not
successful, it is recommended to synthesize by yourself"`.

**For your port this inverts nicely.** Stitching on the ARM box is strictly
better than stitching on a phone: the frames are already local so nothing has to
cross the wire twice, the config YAML is right there, the result never needs
uploading back, and you can report real progress. This is the clearest case in
the whole app where running on the device is not a constraint but an advantage.

---

# 5. UX conventions worth copying

## 5.1 Navigation model

**There is one screen.** The live view is the app; everything else is an overlay
on it. 64 dialogs, 12 mode panels, one activity. Overlays come in three shapes:

1. **Bottom sheets** — the mode picker, settings, parameter dialogs. They slide
   up over 150 ms, dim nothing, and dismiss by tapping outside or by an explicit
   `"Back"` / `"Complete"` in their own header. Every sheet carries a
   `BottomGestureNullView` to swallow the system gesture area.
2. **Top banners** — the hint slot (§2.6). Non-modal, one at a time.
3. **True modals** — only for physical state the user must not interrupt:
   `MovingGimbalDialog` (`"Moving to selected spot"`), `ProcessingDialog`
   (`"Image is being processed"`), the calibration sequence. All three are
   Lottie animations with a single line of text and no dismiss control.

Reaching a setting never leaves the live view. Copy this. A web app that
navigates away from the video to change ISO will feel much worse than the phone.

## 5.2 Visual grammar

Pure dark, and not a themed dark — a black-canvas UI designed to sit on a video
feed at night without wrecking dark adaptation.

| Token | Value | Role |
|---|---|---|
| background | `#000000` | the video is the background; chrome floats on it |
| surface | `#1C252F` at 10/34/50/68/80/86% alpha | every panel, pill and sheet — a cool slate, never neutral grey |
| primary text | `#FFFFFF` | |
| secondary text | `#FFFFFF` @ 60% (`#99FFFFFF`) | |
| hint / annotation text | `#B8CDE5` | a desaturated blue, used inside instructional HTML |
| accent | `#00D1FF` | cyan |
| brand accent | `Coral` `#FF7F50` | `colorPrimary` |
| ripple | `#0085FF` @ 30% | |

(`res/values/colors.xml`; theme `MyAppTheme` in `res/values/styles.xml` —
`Theme.Material3.Light.NoActionBar` with `windowBackground #ff000000`,
`windowFullscreen true`, all elevation forced to `0dp`.)

Elevation is zero everywhere: depth is expressed purely as **translucent slate
over video**, never as a shadow. Custom text views (`RebotRegularTextView`,
`RebotBoldTextView`, `CAI9789TextView`) carry an `app:frameShow` / `frameColor` /
`frameSize` attribute that draws a 1 dp outline behind glyphs — that is how white
text stays legible over a bright sky. **Reproduce it with `text-shadow` or
`paint-order: stroke`**; without it, readouts vanish against snow and cloud.

## 5.3 Value formatting

Copy these exactly — they are what makes the numbers look native:

- Shutter: `1/40`, plain fraction, no unit
- Aperture: `F2.6` — capital F, no space, no `/`
- EV: `+1.6` — always signed, one decimal
- ISO: `40000` — bare integer, no separators
- WB: `AUTO` in caps, or an icon
- Battery: `NN%`
- Video elapsed: `00:00:00`
- Settling time: `0s` — integer seconds with a bare `s`
- Angles: `°` suffix, e.g. `"Clockwise 90°"`, `"0°"`
- Coordinates: user-switchable `D.D` ↔ `D.M.S`, with the switch labelled by its
  *destination* — `"Switch to D.D"` / `"Switch to D.M.S"`

## 5.4 How state and errors surface

Four tiers, used consistently:

1. **Toast** for a change the user just made — speed gear, People Remover on/off.
2. **Top banner** for a condition that blocks something, always carrying the fix
   as a link (§2.6).
3. **Inline coach-mark** — a green callout with an up-triangle pointer, anchored
   to the control it explains, with a `"Do Not Show Again"` / `"No longer
   prompt"` checkbox. Used for auto-level and cable-release.
4. **Modal** only while hardware is physically busy.

Dimmed-but-present is the standard "unavailable" state, and tapping a dimmed
control **explains why** rather than doing nothing — `"Auto Level can not be
accessed in this feature."` (`BatteryMemoryLayout.java:150-158`). That single
behaviour is worth more than any amount of styling.

## 5.5 Confirmation patterns for motor and destructive actions

Three distinct escalation levels, chosen by consequence:

- **Gesture guard** — the shutter/record slide (§2.4). No dialogue; the gesture
  itself is the confirmation. Used for the highest-frequency risky action.
- **Two-button dialogue** titled `"Safety Tip"` (`saft_warning`) with
  `"Cancel"` / `"Confirm"` (`dialog_common_confirm_cancel.xml`). Body text names
  the physical risk in plain language, e.g. `"Because the restricted angle has
  been enabled, the Roll and the Pan Motors might bump into each other. Please
  pay attention to the distance between the Roll and Pan Motors in real time. If
  contact is about to occur, please turn off the Polaris immediately, readjust
  the Astro Kit mounting position, and then turn on the Polaris again for use."`
- **Physical attestation** — the strongest, and the most interesting. To enable
  `"Enable Restricted Angle"` the user must confirm they have physically aligned
  a screw, and the confirm button is worded as the attestation itself:
  **`"Aligned Manually. Enable It"`** (`dialog_open_angle_limit.xml`). Same
  pattern for L-Bracket correction: **`"Confirmed, Turn It On"`**.

That last idiom — *the button states what the user is asserting, not what the
app will do* — is the best thing in the app. It survives a language barrier and
it makes the user's responsibility explicit. Reuse it verbatim for anything that
lets the head swing into a place it could otherwise collide.

The head enforces its own limits too, and reports them: `"Polaris has reached
its rotational limit"` (`gimbal_err_angle_limit_hint`). Surface that in the web
UI rather than letting a joystick silently stop responding.

Acknowledgement dialogues use one button labelled `"Continue"` (`m_i_know`, a
literal translation of 我知道了). It reads as "next", not "I understand" —
**use `"Got it"`**.

## 5.6 What makes it feel good

- **One screen, overlays only.** You never lose the picture.
- **The hint banner always carries its own fix.** `"SD Card Error…"` →
  `"Format SD Card"`. Never a dead-end error.
- **Double-tap a joystick to recentre that axis.** Instant, obvious once seen,
  no menu.
- **Dimmed controls explain themselves when tapped.**
- **Physical-attestation button labels** for collision-risk settings.
- **Slide-to-fire** on the shutter — accidental capture during framing is a real
  problem and the guard is correct in principle.
- **Scroll pickers for exposure**, not steppers — a flick crosses the whole ISO
  range.
- **Reserved slots**: the shutter picker goes `INVISIBLE`, not `GONE`, so the
  strip never reflows as the exposure mode changes.
- **Settling time is a first-class setting** with an explanation of *why* it
  exists (inertial oscillation blurring pano frames). The app teaches, not just
  configures.
- **Committed-value writes**: the settling-time slider sends on touch-up, not on
  every drag frame (`InnerSettingDialog.java:329-337`). Do the same or you will
  flood the socket.
- **`keepScreenOn`** on the control screen. The browser equivalent is the
  Screen Wake Lock API — do it.
- **The PATH-LAPSE `+` interlock.** After the first keyframe the `+` button
  greys out and *only a joystick touch revives it*, while the hint text cycles
  through three states telling you exactly what to do next. It makes an
  unfamiliar two-step gesture self-teaching without a tutorial. The best
  interaction in the app.
- **`"Preview"` rehearses a PATH-LAPSE path** before committing to the run,
  showing `"Previewing the track"` with a one-tap cancel. Cheap to port,
  enormously reassuring.
- **The `"Option 1"` / `"Option 2"` interval labels *are the formulas*.** Rather
  than naming the two policies something opaque, the app writes out
  `actual = set + shutter + processing`. Ugly, and completely unambiguous.
- **HDR's three tiles say what each frame is *for*** — `"Will preserve bright
  details in the image"` — not just `-1 EV`.
- **Holy Grail's four-handle shutter slider** puts a working span inside a
  safety span on one control. Sophisticated and legible.
- **Operating preconditions are stated where they bite.** Holy Grail spells out
  that the camera must be in M mode with ISO-auto off and Auto Review disabled,
  instead of failing mysteriously.

## 5.7 What is clumsy — do not copy

- **The slide-to-shoot gesture.** Right intent, wrong mechanic: it is a 54 x 149
  dp vertical drag with an invisible commit threshold at one third of the
  height, and no visible track or label saying which way to go. On a trackpad or
  a browser touch surface it is worse still. **Keep the guard, change the
  mechanic** — press-and-hold with a filling ring, or a tap that arms plus a
  confirm, both of which read at a glance.
- **Tap-a-joystick-to-become-a-D-pad.** A single tap silently swaps the entire
  control paradigm after a 150 ms delay, and touching anywhere else swaps it
  back. It is undiscoverable, and it is an accidental-activation trap while
  framing. Make it an explicit, persistent toggle.
- **The status row mixes readouts and controls with no visual distinction.**
  Battery, SD and Wi-Fi look exactly like the People Remover, hide-preview and
  auto-level *buttons*. Separate them, or give the interactive ones a chip
  background.
- **`"Continue"` for everything.** The same word acknowledges a hint, dismisses a
  warning and advances a wizard. Use distinct verbs.
- **The Polaris "Shooting Tips" sheet ships Theta's copy.** `ShootingHelpDialog`
  inflates `shooting_help_layout.xml` (`ShootingHelpDialog.java:42`), whose 22
  string resources read `"…inserted in both the camera and the Theta."`,
  `"Theta APP only supports controlling motorized lens"`, and so on. Treat
  every factual claim in that sheet as **Theta's**, not the Polaris's — including
  the 16-image auto-stitch ceiling and the "Stack mode cannot be auto-stitched"
  note. Do not carry those numbers over without verifying them against the
  Polaris.
- **Typos in shipped identifiers and copy** — `Acitivty`, `saft_warning`,
  `foucstraklayout`, `"opertaions"`, `"roration"`, `"shooing"`. Cosmetic, but a
  sign the strings were never proofread; do not mirror them when you reuse the
  vocabulary.
- **Class names that contradict the UI** — `FocusTrackLayout` is focus
  *stacking*; `Precompile` is `FREE PROGRAM`. Name your modules after the labels.
- **Two rendering paths for live view.** The app carries a whole ijkplayer/ffmpeg
  stack to handle both MJPEG and RTSP. You need neither; an `<img>` tag on
  `:8080/?action=stream` is the entire feature.
- **Head-side settings buried inside help dialogues.** `"Tilt Compensation"`,
  `"Dithering"` and the full-vs-half tracking rate are only reachable from the
  ASTRO help sheet's `"Basic Settings"` tab. These change tracking accuracy;
  they belong on the main surface, not three taps deep behind a `?` icon.
- **Holy Grail saves by closing.** Nine separate commands fire on dialogue
  dismiss. There is no Save button, no Cancel, and no way to back out of an
  edit. Give it an explicit commit.
- **The security model.** The head's Wi-Fi AP is **open**, the device password
  is handed to any client that asks for it, it is Base64 rather than hashed, and
  every check happens in the app. Faithfully porting this reproduces a null
  security model. If your web app is reachable beyond the head's own AP, add
  real authentication.
- **Silent, invisible modal state.** `MovingGimbalDialog` blocks the whole UI
  with `"Moving to selected spot"` and no cancel, no progress and no timeout
  shown. A slew can take a while; give it a cancel.
- **Defaults that do not match intent.** `"Shutter Response Time"` defaults to
  **0 s** on a device whose own help text explains why that causes blurred
  panoramas; TIMELAPSE defaults to **∞ shots**. Pick defaults that produce a
  good first result.
- **Dead and unfinished surfaces shipped.** The privacy-consent dialogue is
  never instantiated, the `"Discover"` tab carries Chinese lorem text and a fake
  `"8 may 2023"` date, `PolarisConnectLayout` is an empty stub, and a whole
  second settings UI (`SettingActivity`, 1,383 lines) survives behind a hidden
  long-press. A hardcoded Chinese string also sits in the cellular-failure
  dialogue's layout.

---

# 6. Prioritised port list

The target is a web app **running on the Polaris itself**, served to any
browser. That changes the calculus in three ways:

- **The head is local.** No BLE, no Wi-Fi join, no dual-network routing, no
  cellular relay. The hardest parts of the phone app's connection stack simply
  vanish.
- **The browser has no sensors.** No magnetometer, no reliable GPS, no camera,
  no vibration on desktop. Anything built on the handset's own hardware must be
  redesigned, not ported.
- **A page can be open on several devices at once**, and the head's control
  socket is a single TCP connection. Your server owns that socket and fans out;
  the browser never speaks it directly.

That last point deserves emphasis, because read the wrong way it looks like a
blocker. A browser cannot open a raw TCP socket, the head serves no CORS
headers, and it is cleartext HTTP — so a page fetched *from the head* could not
drive it. But that is not this architecture. **Your server process holds the
`:9090` socket and the HTTP client, and exposes whatever the browser needs**
(WebSocket, SSE, JSON). The browser only ever talks to you. The one thing that
does pass through untouched is the MJPEG stream, which an `<img>` renders
natively — and even that you may want to proxy so the head sees a single client.

## 6a. Core — the web app cannot replace the phone without these

| Feature | Why it is core |
|---|---|
| **Live view** | `<img src="http://…:8080/?action=stream">`. The whole feature, one tag. Everything else is framing on top of it. |
| **Pan/tilt/roll jog with a speed control** | The single most-used control. Reproduce the **velocity** semantics (`SP_GIMBAL_*ADJ_SPEED`, release = stop), the 1–5 speed gears, and the constrained single-axis sticks on a 2-axis head. Add keyboard arrows — free on a browser, impossible on the phone. |
| **Double-tap to recentre an axis** | `SP_GIMBAL_POS_RESET`. One line of code, disproportionate value. |
| **Camera settings strip: S / F / EV / ISO / WB + AF/MF** | **Fetch the option lists from the camera** (`SP_GET_SHUTTER_INFO`, `SP_GET_ISO_INFO`, `SP_GET_FNUM_INFO`, `SP_GET_EV_INFO`, `SP_GET_WB_INFO`) — never hardcode them. Set by string value. |
| **Shutter / record, with a deliberate guard** | Keep the intent of slide-to-fire; change the mechanic (§5.7). |
| **Status: battery, SD free, camera identity, connection** | Plus the 20 % banner and the ≤16 % modal. Add a shots-remaining estimate — the app has none, and it is trivial from free MB and average file size. |
| **The top hint banner** | The pattern, not just the strings. Every blocking condition carries its own fix. |
| **Error-code surfacing** | The `-1002` / `-1005` / `-1203` table in §4.8, verbatim. Silent failure is the worst outcome for a remote-controlled camera. |
| **PHOTO and VIDEO** | Baseline. Remaining-shots readout and an elapsed timer. |
| **PANO (normal and PRO)** | The flagship program and the reason most people bought the head. Port the FOV math exactly, including both width/height swaps. |
| **TIMELAPSE** | Second flagship. Port both interval policies, and fix the duration estimate. |
| **Manual GPS entry (D.D and D.M.S)** | On a headless box this is **mandatory**, not optional — the sky model cannot run without a location, and there is no phone to ask. |
| **Auto Level** | Device-side, one command, high value. |
| **Media browse, view and delete** | Delete needs a confirmation *and* a result check — the original has neither (§4.10). |
| **Server-generated thumbnails** | Core, not optional. The head has **no thumbnail endpoint**: every grid tile in the phone app is a full-resolution download, serialised one at a time. You are running next to the files — generate real thumbnails and the gallery stops being painful. |
| **Angle-limit / safety-limit state** | Including the physical-attestation confirmation. A UI that lets the head swing into its own accessory is worse than no UI. |
| **Session keep-alive and reconnect** | With `"Auto OFF"` enabled the head powers itself down after **10 or 30 minutes** unused (`"The device will automatically power off after being unused for more than 10 mins."`), and its Wi-Fi sleeps shortly after the client disconnects — see `docs/ASTRO.md`. A browser tab left open is not "use". Hold the socket server-side and surface the state honestly. |

## 6b. Valuable — port next

| Feature | Note |
|---|---|
| **PATH-LAPSE** | Keyframe path, 8 points, plus the `+` interlock and the `"Preview"` rehearsal. Fix the `bulb:0` bug while you are there. |
| **The shot-position grid for PANO** | And improve it: the original has **no thumbnails and no per-cell shot/pending/failed state**. Adding those is the biggest single UX win available. |
| **HDR** | Three fixed frames, per-frame overrides. Simple to port; consider allowing 5 and 7 frames, which the head may well accept. |
| **ASTRO GOTO and tracking** | High value but gated on solving alignment (§6d) and on the Astro Kit for tracking. The catalog can ship as static JSON. |
| **The on-screen settings sheet** | Especially `"Shutter Response Time"` (with a better default), the two interval policies, and return-to-start. |
| **Media browse and download** | Over WebDAV at `/dav`, or the existing HTTP resource root. |
| **Histogram, grid lines, focus peaking overlays** | Focus peaking needs pixel access to the MJPEG frames — a canvas draw, entirely feasible in-browser, and arguably better than the app's version. |
| **Presets — with names** | The app's panorama presets are unnamed and undeduplicated. Naming them is a one-line improvement. |
| **FOCUS STACK** | See §3.8. |
| **FREE PROGRAM (the keyframe timeline)** | The most expressive feature in the app — and it **cannot save a program** (§3.9). Add save/load and it becomes genuinely useful. Its scheduled-start already runs head-side with the phone disconnected. |
| **On-device stitching** | The head supplies a solved camera model (`ISP_stitching_config.yaml`) derived from its own encoders, which is why the stitch is fast and works on featureless sky. Doing it on the box beats doing it on a phone outright: frames are already local, nothing uploads back, and you can show real progress. Write `GPano:*` XMP while you are there — the original writes none, so no sphere viewer recognises its output. |
| **Multi-client awareness** | Two browsers on one head is a new situation the phone app never had. At minimum, show who holds control. |

## 6c. Niche or skip

| Feature | Why |
|---|---|
| **SUN mode's calendar reminders** | No browser equivalent. Offer a downloadable `.ics` instead. |
| **Holy Grail** | Needs the **Optical Matrix Sensor Module** accessory; without it there is nothing to ramp from. Skip unless you own one — or build your own metering from downloaded JPEG histograms, which is a *different* design, not a port. |
| **720 PANO** | Fixed 360°×157° with an undocumented `+2` zenith/nadir frame count and a preview grid that does not show them. Low value, high confusion. |
| **People Remover** | A head-side median stack. One toggle, no configuration — port the toggle, ignore the rest. |
| **Cellular / remote-control relay** | The FRP proxy and MQTT wake exist so a *phone* can reach a *distant* head. Your server is already next to the head. Skip entirely. |
| **BLE discovery and Wi-Fi join** | Replaced by "the server is on the device". If you do need to wake the head's AP from a cold state, that is a host-side BlueZ concern, not a browser one. |
| **The device password / security questions** | Do not port it. It is Base64, handed out on request, and checked client-side. Put real auth in front of the web app instead. |
| **Firmware update** | The app downloads from Benro's cloud and re-uploads over Wi-Fi. On-device you can fetch directly — but this repository already owns the firmware story, so defer to it. |
| **Analytics / Facebook / Crashlytics** | Bundled but never called. Nothing to port. |
| **The privacy-consent dialogue** | Dead code, never shown. |
| **The `"Discover"` tab** | Ships placeholder Chinese lorem and a fake date. Unfinished. |
| **The legacy shell and legacy settings** | A whole second UI behind a hidden long-press. Ignore. |
| **App self-update** | Irrelevant — a web app deploys by being served. |
| **FREE PROGRAM's calendar reminders** | Two hazards: it invents a calendar account named `boohee` if none exists, and it **deletes every event titled `"Reminder"` or `"Shooting Complete"`** when cancelling. A server-side scheduler is safer and better. |
| **The cellular download warning** | It keys on connection mode rather than the actual radio, so it is wrong even on the phone. |

## 6d. Phone-only dependencies, and what to do instead

These are the parts that **cannot** be ported and must be redesigned. Call them
out early in your build, because two of them sit on the critical path.

| Depends on | Used for | What to do instead |
|---|---|---|
| **Phone magnetometer + accelerometer** | `CalibrationDialog` — establishing the head's true-north heading, then `SP_SET_YAW(heading, lat, lng)` | **The critical one.** Browsers have no reliable absolute compass: `webkitCompassHeading` is iOS-Safari-only, Android Chrome exposes no absolute heading, and desktop has nothing. Three options, best first: **(1) skip it** — one-star alignment (`SP_CALIBRATE_START` 1→2→3) fully determines the sky frame on its own, and `SP_SET_YAW` is only a coarse pre-seed that makes the first GOTO land closer; **(2) plate-solve** — this repository's `astro-plate-solving` branch already does exactly this, and it is strictly more accurate than a phone compass; **(3) let the user type a bearing.** Note the app's own instruction is to hold the phone *against* the head, so the phone is merely a proxy for the head's heading — any bearing source is equally valid. |
| **Phone GPS** | Location for the sky model, rise/set times, and `SP_SET_YAW` | `navigator.geolocation` over HTTPS works, but on a headless box the reliable answer is the **manual D.D / D.M.S entry the app already has** — port it and make it primary. Better still, persist it: a tripod head usually lives at a handful of known sites. |
| **Phone camera** | Nothing. | Already the head's MJPEG stream. No action. |
| **Phone Wi-Fi RSSI** | The signal-strength icon in the status row | Read it host-side from the interface, or drop it. |
| **Phone vibration** | Haptic ticks on the joystick, shutter and focus jog | `navigator.vibrate` on mobile; on desktop substitute a visual detent or a short click. Do not lose the feedback entirely — it is what makes the joystick feel mechanical. |
| **Phone calendar** | SUN-mode reminders | Downloadable `.ics`, or a server-side scheduled job — which is *better*, since the head can then run the capture whether or not a browser is open. |
| **Phone Bluetooth** | Discovery and the AP wake | Host-side BlueZ if a cold wake is needed; otherwise not applicable. |
| **Android system-settings deep links** | `"Turn on WiFi"`, `"Turn on GPS"` rows | Your own admin page on the box. |
| **Play Store / APK sideload** | App self-update | Not applicable. |

**One genuine new capability**, worth designing for from the start: because the
server runs on the head, a program can keep running with **no client connected**.
The phone app cannot do that — closing it ends the session. A scheduled
sunrise time-lapse that fires whether or not anyone is watching is the single
most compelling reason to build this at all.
