# The Benro Polaris app protocol

A wire-level reference for the TCP control protocol the Benro Connect phone app
speaks to the Polaris head, reconstructed by static analysis of the decompiled
Android app and cross-checked against two independent implementations.

This is interoperability documentation for hardware you own. It exists so an
on-device web control panel can do what the phone app does without the phone.

> **Read [Danger list](#8-danger-list) before exposing any of this in a UI.**
> Several commands move motors with no user confirmation, one disables the
> mount's own travel limits, and several write flash.

---

## Provenance and confidence

Three independent sources agree on the parts where they overlap. Where they
disagree, or where only one covers something, this document says so.

| Source | What it is | Weight |
|---|---|---|
| **APK** | `BenroConnect` decompiled to Java (JADX). The app's own method names and a constants class survived, so most command names below are the vendor's, not guesses. | Primary — but static only |
| **`polaris-mount.c`** | This repo's C client, six commands, field-tested on hardware | Authoritative for the six it uses |
| **alpaca-benro-polaris** | Independent open-source ASCOM driver, field-tested by its author | Authoritative where it overlaps |
| **`docs/ASTRO.md`** | This repo's own device-log captures from the running firmware | Authoritative — it observed the firmware parsing frames |

### Citation shorthand

| Short | Path |
|---|---|
| `POC:N` | `…/sources/com/snoppa/polaris/singleton/PolarisOrderCommunication.java` line N |
| `CMD:N` | `…/sources/com/snoppa/application/constant/polaris/PolarisCMD.java` line N |
| `MAIN:N` | `…/sources/com/snoppa/polaris/activity/MainActivity.java` line N |
| `SKY:N` | `…/sources/com/snoppa/application/opengl/DrawSkyTools.java` line N |
| `SH:N` | `…/sources/com/snoppa/application/oksocket/SocketHelper.java` line N |
| `WSH:N` | `…/sources/com/snoppa/application/oksocket/WifiSocketHelper.java` line N |
| `WB:N` | `…/sources/com/snoppa/application/singleton/WifiBroadcast.java` line N |
| `OPT:N` | `…/sources/com/snoppa/application/oksocket/client/sdk/client/OkSocketOptions.java` line N |

Root for all of the above:
`/Users/blainemiller/Documents/scripts/Research/Aperion/Benro Polaris Software/BenroConnect_decompiled/sources/`

For the independent driver, rooted at
`/Users/blainemiller/Documents/scripts/Research/Aperion/alpaca-benro-polaris/`:

| Short | Path |
|---|---|
| `ALP:N` | `driver/polaris.py` line N |
| `CTL:N` | `driver/control.py` line N |
| `BLE:N` | `driver/ble_service.py` line N |

**Duplicate-file note.** That tree contains iCloud `* 2.java` copies.
`PolarisOrderCommunication.java` and `PolarisOrderCommunication 2.java` are
**byte-identical** (`diff` returns clean), as are `PolarisCMD.java` and its
duplicate. Every citation here is to the primary file.

**Naming note.** Names in `SP_*` form are the **app's own** — they come from
`PolarisCMD` (CMD:24–144) or from the method names in `PolarisOrderCommunication`,
which the obfuscator left intact. Anywhere a name or meaning is *inferred*, it is
marked **(inferred)**.

---

## 1. Transport

### Endpoint

The head is an access point and nothing else by default. It has **no station
mode** — stock firmware cannot join another network (this repo's
[NETWORKING.md](NETWORKING.md) adds one; the alpaca driver's
`docs/troubleshooting.md:123` confirms stock cannot).

| | |
|---|---|
| Host | `192.168.0.1` — the head's own AP address |
| SSID | `polaris_<serial>` — prefix constant `WifiContainPolaris = "polaris_"` (CMD:152) |
| Encoding | ASCII, no TLS, no compression |

Hardcoded in the app at SH:104–107, mirrored in
`application/singleton/AppGlobalDataMgr.java:74–77`. The alpaca driver's
`driver/config.toml:5–6` uses the same address and port.

**Ports the head serves:**

| Port | Proto | Purpose | Cite |
|---|---|---|---|
| **9090** | TCP | **The control protocol** — everything in this document | SH:105 |
| 8080 | HTTP | MJPEG live preview, `GET /?action=stream` | SH:107; MAIN:2483 |
| 8080 | HTTP | RSSI sink — the *app pushes its measured signal strength to the head*, `GET /?action=signal&<0-50>` | `application/utils/UtilFunction.java:417`; MAIN:3176 |
| 8554 | RTSP | HDMI capture stream, `rtsp://192.168.0.1:8554/12` | `application/constant/ApplicationConstants.java:15` |
| 80 | HTTP | Media download (`/…`), WebDAV (`/dav/…`), and `POST /sd` upload | `polaris/singleton/PolarisSyntheticHelper.java:165, 934`; `application/utils/UgradeUtils.java:490` |

`polaris-mount.c` defaults to `127.0.0.1:9090` because it runs *on* the device.

**There is exactly one control socket.** Telemetry (517, 518, 525) is
interleaved with command replies on it — there is no separate telemetry port.
The alpaca driver has a single `open_connection` in the whole tree (ALP:306).

### The access point is open

**No passphrase.** The app builds its Wi-Fi config with
`allowedKeyManagement.set(0)` (= `KeyMgmt.NONE`) and never assigns
`preSharedKey` anywhere in the tree
(`application/utils/PhoneConnectUtils.java:58–69`). The modern API-29 path
likewise omits `setWpa2Passphrase` (WB:1389–1396).

**The app also self-assigns a static IP** rather than using DHCP —
`192.168.0.<random 10–249>/24`, gateway and both DNS at `192.168.0.1`
(`PhoneConnectUtils.java:63–68`). A client that expects DHCP may or may not get
a lease; assigning statically is the proven path.

### Discovery is BLE, not the network

There is **no UDP broadcast, no multicast, and no mDNS**. A sweep for
`DatagramSocket`, `MulticastSocket`, `NsdManager` and `255.255.255.255` across
the app returns nothing.

The class named `WifiBroadcast` is an Android `BroadcastReceiver` for system
intents (WB:102), not a network broadcaster — an easy misread.

The head is found by its **BLE advertisement name** (prefix `polaris_`,
WB:396–406), and its Wi-Fi BSSID is then computed arithmetically from the BLE
MAC by incrementing the last nibble
(`application/utils/PhoneConnectUtils.java:102–109`, used at WB:437).

### The 60-second radio timer

The firmware powers the Wi-Fi radio down **60 s after the last registered client
disconnects** and unloads the driver. This is stock behaviour with no setting,
captured from the device log in [ASTRO.md](ASTRO.md):

```
01:41:16  SP_ClientCtxDel: id[3], type[wifi]; WifiCount[0]
01:42:16  WifiBtTask[201]: wifi auto off
01:42:17  MsgFromWifiBt --> val[wifi:0; bt:1;]
```

A plain TCP connection **does not count as a client**. See
[§4 Registering as a client](#registering-as-a-client) — this is the single most
important thing to get right for a headless controller.

### Bluetooth — discovery and wake only

The last log line above (`wifi:0; bt:1;`) shows Bluetooth staying up as the wake
path after Wi-Fi sleeps. **No control traffic runs over BLE.** Every command in
this document is TCP. Both independent sources agree, and neither carries Wi-Fi
credentials over BLE (the AP is open, so there are none to carry).

The app uses **RxAndroidBle** (`com.polidea.rxandroidble2`), and only from WB.
For a Polaris it scans adverts by name prefix (WB:374–406) and then performs a
**connect-and-immediately-disconnect GATT dance purely to wake the Wi-Fi AP** —
`tryBleAwakenWifi()` at WB:1150–1189 calls `connectWifi(...)` on
`STATE_CONNECTED` and then `close()`/`disconnect()`s at WB:1180–1184. **It never
calls `discoverServices()` and never writes a characteristic.**

The alpaca driver reaches the same goal differently, and its method is the more
useful one for a headless controller — it writes a single literal to a GATT
characteristic (BLE:299–301):

```python
await client.write_gatt_char(SEND_UUID, b"enable_wifi", response=use_response)
```

| | UUID |
|---|---|
| Send characteristic | `0000fff1-0000-1000-8000-00805f9b34fb` (BLE:10) |
| Receive characteristic | `0000fff2-0000-1000-8000-00805f9b34fb` (BLE:11) |

The driver scans for adverts whose name starts with `polaris` (BLE:111) and
retries `enable_wifi` every 30 s **only while TCP is down** (BLE:145–149). It
reads back from `fff2` afterwards but **nothing parses the response** (BLE:309).

> **Attribution caution.** These same `fff0`/`fff1`/`fff2` UUIDs appear in the
> app as constants explicitly named `ThetaBLEUUID_*` (WB:109–111) and are
> resolved **only** in the Theta code path. The app's own routing sends
> `polaris_` devices down the Wi-Fi path and only `theta_live` down the GATT
> path (`application/utils/BenroDeviceUtils.java:35–61`). So the app treats
> these UUIDs as Theta's — yet the alpaca driver uses `fff1` successfully on a
> Polaris. Both wake the radio. Treat `enable_wifi` on `fff1` as **field-proven
> but not corroborated by the app**.

The app's Theta path does send SSID and password in plaintext over BLE
(`application/utils/BLEUtil.java:258–263`) — that is **Theta Live joining the
user's home network**, and has nothing to do with the Polaris.

---

## 2. Framing

### Request

```
1&<code>&<type>&<key>:<value>;<key>:<value>;…#
```

Built at **POC:1443**:

```java
String str2 = "1&" + i + "&" + i2 + "&" + str + MqttTopic.MULTI_LEVEL_WILDCARD;
```

`MqttTopic.MULTI_LEVEL_WILDCARD` is the string `"#"` — the app borrows the
constant from the bundled Paho MQTT library purely to get a `#` character. It has
nothing to do with MQTT.

| Field | Meaning |
|---|---|
| `1` | **Fixed literal.** Meaning not determined statically — see [§9](#9-open-questions). The same builder with the same leading `1` is used for the unrelated Theta product (`theta/singleton/ThetaOrderCommunication.java:619`), so it is a protocol-level constant, not a Polaris command field. |
| `<code>` | Numeric command id — the `i` parameter |
| `<type>` | **Subsystem selector, 1–4.** The `i2` parameter. **This is not always 3.** |
| `<key>:<value>;…` | Payload. Trailing `;` on the last pair is normal. |
| `#` | Terminator |

> **Correction to `polaris-mount.c`.** The comment at
> `container/astro/polaris-mount.c:6` describes the format as
> `1&<cmd>&3&<k>:<v>;...#`. The `3` is not constant — it is a per-command
> subsystem field. The C code's *actual behaviour* is already correct (it sends
> `&3&` for 517/519/527/530/531 and `&2&` for 284/520, which is what the app
> does); only the comment over-generalises.

The `<type>` values, derived from every one of the 185 `sendOrder` call sites in
`PolarisOrderCommunication`:

| `type` | Subsystem | Command ids |
|---|---|---|
| `1` | Camera exposure parameters | 258–262, 265–268, 275, 276, 311 |
| `2` | Application / system / creative programs | 263, 264, 270–272, 277, 280, 283–286†, 289, 291, 292, 296–307, **520**, **526**, 770–825 |
| `3` | Gimbal / motion | 513–519, 521–524, 527, 530–549 |
| `4` | Camera capability query | 282, 286 |

† 286 is sent with `type:4` (POC:1079); 285 with `type:2` (POC:853).

**Note the two exceptions in the 5xx range:** `520` (POC:903) and `526`
(POC:1136) are sent with `type:2`, not `3`, despite their ids sitting among the
motion commands. Use the per-command table in §5, not the id range.

> **The `type` field appears to be advisory.** The alpaca driver sends **802**
> and **824** with `type:3` where the app sends both with `type:2` (ALP:1058,
> ALP:1046 vs POC:1103, POC:1428), and it works in the field. Similarly
> `polaris-mount.c:329` reasons that "the third field is 2 for queries and 3 for
> commands" — a rule that does not survive contact with 517, a query sent with
> `type:3`. The safest course is to copy the app's value per command from §5;
> the evidence suggests a mismatch is tolerated, but nothing proves it is
> tolerated for *every* command.

The firmware's own parse of this frame, captured in ASTRO.md, confirms the field
names:

```
rcv msg from App[5]: type:2; code:808; val:type:0;
```

That was the wire bytes `1&808&2&type:0;#`. So the firmware calls field 2
`code`, field 3 `type`, and the remainder `val`. The leading `1` is consumed
before any of them.

### Reply

```
<code>@<key>:<value>;<key>:<value>;…#
```

The reply **omits the `type` field**. The app's receive callback is
`parseSocketCMD(int i, String str)` (POC:69) — `i` is the code parsed from
before the `@`, `str` is everything between `@` and `#`.

### Stream framing — `#` terminated, not length prefixed

This matters because the app's socket library would normally do something else.
It is xuhao's **OkSocket**, vendored into `com.snoppa.application.oksocket`, and
it is configured with `DefaultNormalReaderProtocol` — a 4-byte big-endian length
header (OPT:227). **That path is dead code.** The reader was patched to a raw
drain-what's-available loop (`…/oksocket/core/iocore/ReaderImpl.java:39–50`),
and the entire stock header/body protocol now sits inside the
`catch (IOException)` block, unreachable in normal operation. The writer
correspondingly emits the payload verbatim with no header
(`…/oksocket/core/iocore/WriterImpl.java:43–46`).

So the wire is **pure UTF-8 text** and all framing is at the application layer.
The splitter is WSH:168–203:

```java
this.resultBuilder.append(str);                                   // :175
if (!str.endsWith(MqttTopic.MULTI_LEVEL_WILDCARD)) return;        // :176  endsWith("#")
String[] strArrSplit = this.resultBuilder.toString().split(...);  // :180  split on "#"
…
String[] strArrSplit2 = this.messageString[i].split("@");         // :187
if (strArrSplit2.length > 1)
    socketHelperListener.parseSocketCMD(Integer.parseInt(strArrSplit2[0]),
                                        this.parseString[1]);     // :193
…
sb.delete(0, sb.length());                                        // :201-202
```

> **Do not copy this reassembly.** It accumulates until a read chunk happens to
> *end* with `#`, then splits the whole accumulator and **clears it
> unconditionally** (WSH:201–202). A trailing partial message in the same
> accumulator is silently discarded. Buffer properly and consume up to each `#`
> instead — which is what `polaris-mount.c:249–281` does.

Messages without an `@` are silently dropped by the `length > 1` guard
(WSH:189).

**Command ids are assumed to be exactly three digits** by both independent
implementations — `polaris-mount.c:262` does `memcpy(cmd_out, at - 3, 3)` and
the alpaca driver's regex is `r'(\d{3})@(.+?)#'` (ALP:83). Every id observed is
in 258–825, so this holds today, but it is an assumption, not a guarantee.

### Payload parsing rules — three traps

These come from the app's own parsers and are easy to get wrong:

1. **Values are taken after the LAST colon, not the first.** Every parser does
   `strTrim.substring(strTrim.lastIndexOf(":") + 1)` — e.g. POC:2595, POC:2543,
   POC:2405. A value that itself contains a colon (a time like `12:30:00`) is
   therefore truncated to its last segment by the app. Emit values without
   internal colons; when *parsing* replies, splitting on the first colon is the
   safer choice for your own code, but be aware the vendor app does not.

2. **Keys can repeat, and only the first occurrence wins.** `parseSP_PUSH_ROTATE_VECTOR`
   (POC:2596–2620) guards each of `w`/`x`/`y`/`z` with a boolean so that later
   duplicates are ignored. This strongly implies command 518's payload carries
   **more than one** `x:`/`y:`/`z:` triple on the wire and the app deliberately
   reads only the first. Do the same.

3. **Empty segments occur on the wire and are tolerated.** The timelapse sender
   at POC:793 emits a double semicolon:
   `"step:3;point:%d;time:%d;photoCnt:%d;;preview:%d;"`.

   **This is not a decompiler artifact.** Three independent lines of evidence:
   the literal is a plain constant concatenation with no elided value; the same
   `;;` arises from a *different* mechanism where a fragment beginning with `;`
   is appended to a `step:N;` prefix, producing `step:3;;num:5;isp:0;` and
   `step:8;;num:5;` (POC:661, 681, from
   `FocusTrackLayout.getStartShootingParameter()` at
   `polaris/layout/shootingmodel/FocusTrackLayout.java:194`); and every parser
   in the app tolerates empties — `parseSP_PUSH_MODE_STATE` guards explicitly
   with `if (!strTrim2.equals(""))` (POC:2407) while the rest fail their
   `startsWith` tests harmlessly.

   So the format is a **tolerant `key:value;` list in which empty segments are
   no-ops**. Emit it byte-identically if you want bug-compatibility, and skip
   empty segments when parsing.

### The empty payload

`sendOrder` substitutes a literal when the payload is null (POC:1439–1441):

```java
if (str == null) { str = "-100"; }
```

`-100` is `PolarisCMD.EMPTY_CONTENT` (CMD:7). So a pure query such as
`SP_GET_GIMBAL_POS` (POC:986, payload `null`) goes out as `1&517&3&-100#`.

Note that some senders pass `""` rather than `null` — `SP_PUSH_MODE_STATE`
(POC:849) sends `1&284&2&#`. And `polaris-mount.c` sends `1&284&2&-1#`. All
three forms are accepted by the firmware; the payload of a query is ignored.
There is no need to match the app byte-for-byte here.

---

## 3. Reply status and errors

Almost every reply carries a `ret:` key.

| `ret:` | Meaning | Evidence |
|---|---|---|
| `0` | Success | `SUCCEED_FLAG = "0"` (CMD:149); `parseSP_SOCKET_CLIENT_TYPE` posts success as `"0".equals(ret)` (POC:3285); `parseSP_SET_AHRS_STATE` likewise (POC:2669) |
| `1` | In progress / accepted | `BEING_PROCESSED_FLAG = "1"` (CMD:5) |
| `-1` | Rejected | Not a named constant; observed on hardware — a 519 carrying an out-of-range `yaw` is answered `ret:-1` and the motors do not move (`polaris-mount.c:47–50`) |
| `-100000` | Timeout | `TIME_OUT_FLAG = "-100000"` (CMD:151) — an app-side sentinel |

**`ret:` is not a uniform status code.** Several commands overload it as a state
echo rather than success/failure, so a generic "`ret != 0` means error" handler
will misread them. Confirmed per-command meanings from the field-tested driver:

| Cmd | `ret:` | Meaning | Cite |
|---|---|---|---|
| 285 | `0` | Mode applied (only value treated as success) | ALP:673–675 |
| **519** | `1` | **Slew starting** — first of *two* replies | ALP:754 |
| **519** | `2` | **Slew stopping** — second reply | ALP:754 |
| 520 | `1` | AHRS stream enabled | ALP:846 |
| 527 | `0` | Compass alignment accepted | ALP:768 |
| **531** | `1` / `0` | Tracking **on** / **off** — a state echo, not success | ALP:772–783 |
| 808 | `0` | Client registration accepted | ALP:840 |

**A goto returns two replies.** Anything issuing 519 must consume both
(`ret:1` then `ret:2`) or it will desynchronise the reply stream. The alpaca
driver awaits exactly two (ALP:984–992).

**520's success value is contested.** The alpaca driver documents `ret:1`
(ALP:846) while the app's `parseSP_SET_AHRS_STATE` posts success as
`"0".equals(...)` (POC:2669). Do not gate on either; confirm AHRS started by
watching for 518 pushes instead. Flagged in [§9](#9-open-questions).

**One documented inconsistency.** `parseSP_SET_YAW` (command 527) tests
`Integer.parseInt(ret) == 1` (POC:2632) rather than `== 0`, which is the
opposite polarity from every other parser. Either 527 genuinely reports success
as `ret:1`, or this is a bug in the app. Treat 527's `ret` as
**unreliable for success detection** and verify the heading took effect by
reading 518 instead. Flagged in [§9](#9-open-questions).

### Asynchronous error pushes

**797 `SP_ERROR_CODE`** (CMD:32) arrives unsolicited with `errorCode:<n>`
(POC:3174). The numeric error table is **not** in the app — it maps the code to a
localised string resource. Codes must be collected at runtime.

---

## 4. Connection lifecycle

### Handshake

The app's on-connect sequence, in order, from `setWifiConnectState(true)`
(POC:449–455):

| # | Command | Sent as |
|---|---|---|
| 1 | 524 `SP_GIMBAL_EX_AXIS_STA` — is the third axis present? | `1&524&3&-100#` |
| 2 | 782 `SP_SET_SYSTEM_TIME` | `1&782&2&date:…;time:…;zone:…#` |
| 3 | 778 `SP_GET_BAT_STATE` + start 2 s poll | `1&778&2&-100#` |
| 4 | 775 `SP_GET_SD_INFO` + start 2 s poll | `1&775&2&-100#` |
| 5 | 284 `SP_PUSH_MODE_STATE` — read current mode | `1&284&2&#` |
| 6 | 802 `SP_GET_WIFI_BAND` | `1&802&2&-100#` |
| 7 | 824 `SP_OMS_RUN_STATE` | `1&824&2&-100#` |

**None of this is mandatory.** The alpaca driver's connect sequence (ALP:1201–1224)
is a different set in a different order — 790, 799, 296, 300, 298, then 520,
524, 782, 778, 775, 284, 802, 824, 780, 305, 272, 547, 808, 520, 545 — and works
equally well. Order is not load-bearing except where §7 says so.

> **There is no authentication on this socket.** Any client that connects to
> 9090 and sends well-formed frames gets full control. It does not need to send
> 790, does not need to know the device password, and (as far as either
> implementation demonstrates) does not need 808 either — though 808 *is*
> required to stop the radio sleeping. The password machinery protects app
> *features*, is compared **on the phone** (`polaris/dialog/VerificationDialog.java:212`),
> and is Base64-encoded at best; see §5.9.
>
> Treat network access to the head as equivalent to physical control of it. The
> AP is open (§1), so anyone in radio range is on that footing.

`SP_GIMBAL_EX_AXIS_STA`'s reply handler chains straight into
`SP_GET_DEVICE_VERSION` (POC:3023), so 780 follows automatically.

### Registering as a client

**This is the step that keeps the radio alive**, and it is not part of the
sequence above — the app sends it separately (POC:1131):

```
1&808&2&type:0;#          →   808@ret:0;#
```

`type:0` = Wi-Fi client, `type:1` = cellular client. The boolean argument is
`wifiBroadcast.isPolarisCelluarModel` (`libra/fragments/impl/HomeScannerFragmentInitView.java:746`).

The firmware's response, captured in ASTRO.md:

```
SP_MsgSysFromAppProc: client type[0]; id[5];
SP_ClientCtxAdd: id[5]; type[wifi]; WifiCount[1], CellCount[0];
SP_SendMsgToApp: code[808], val[ret:0;]
```

Without this, a held-open TCP connection raises `SP_EVENT_APP_CONNECT`, gets an
id, but is never added to the client table — and the 60 s sleep timer runs
anyway. **Send 808 immediately after connecting** and re-send it on every
reconnect.

### Polling

What the app polls, and how often:

| Command | Interval | Source | Notes |
|---|---|---|---|
| 778 battery | 2000 ms | POC:486 | **Self-cancelling** — the callback stops the timer once the tick counter exceeds 10 (POC:490–493), so it polls ~11 times then stops. The app restarts it on connection-state changes. |
| 775 SD info | 2000 ms | POC:536 | Same self-cancelling pattern (POC:541) |
| 823 OMS battery | 2000 ms | POC:511 | Same pattern; only while an OMS accessory is online |
| 517 gimbal position | caller-supplied | POC:963–975 | `startGetAngelTimer(tag, intervalMs)`. There is a second identical timer `startGetInitAngelTimer` (POC:934) for the initial read. |
| 265/266/267/268/275 camera params | on demand, rate-limited to once per 2000 ms | POC:1465–1473 | `getCanmeraInfo(true)` |

### Heartbeat — the `h#` pulse

**There is a heartbeat, and it is not part of the command protocol.** The app
sends the two-byte string `h#` on a timer and expects the head to echo it back.

| | |
|---|---|
| Payload | `h#` — `…/oksocket/core/iocore/PulseSendData.java:9` |
| Period | 5000 ms — `mPulseFrequency` (OPT:225), floored at 1000 ms by `PulseManager.updateFrequency()` |
| Started | on connect success (WSH:51–56) |
| Echo handling | an inbound token of exactly `h` feeds the watchdog (WSH:184–185) |
| Failure | after 5 unanswered pulses (`mPulseFeedLoseTimes`, OPT:235) the manager force-disconnects with `DogDeadException` (`…/client/impl/client/PulseManager.java:130–134`) |

So an unanswered heartbeat tears the link down in roughly **25 s**.

Note that `h#` shares the `#` terminator but has no `@`, so the splitter routes
it separately (WSH:184) — a naive `<code>@<payload>` parser will see `h` as junk
rather than crashing, which is the correct behaviour anyway.

Neither `polaris-mount.c` nor the alpaca driver sends `h#`, and both hold long
sessions successfully — so the head does **not** appear to require it. The app
uses it to detect a *dead* link quickly. A control panel that wants fast failure
detection should send it; one that does not, need not.

Beyond `h#`, the 808 registration plus an open socket is what holds the
connection alive against the 60 s radio timer.

**Socket options the app uses** (WSH:92–111, OPT:223–241): `TCP_NODELAY` on
(`…/client/impl/client/ConnectionManagerImpl.java:173`), duplex read/write
threads, big-endian, no SSL, and a connect timeout of **900 ms** — the app asks
for 1 s and the library multiplies by 900 rather than 1000
(`ConnectionManagerImpl.java:172`). Reconnect is `NoneReconnect` for the first
attempt, swapped to `DefaultReconnectManager` after the first success (WSH:57),
with an app-level 5 s retry loop above it (WB:795–823).

### Unsolicited pushes

These arrive without a request. A client must be able to receive at any time:

| Code | Name | When |
|---|---|---|
| 518 | `SP_PUSH_ROTATE_VECTOR` | Continuously once AHRS is enabled — see §5.2 |
| 284 | `SP_PUSH_MODE_STATE` | On any mode/program state change |
| 776 | `SP_PUSH_SD_INFO` | On card insert/remove/fill |
| 777 | `SP_PUSH_SD_HINT_ID` | SD warning |
| 779 | `SP_PUSH_BAT_STATE` | Battery change — parsed by the same handler as 778 (POC:178) |
| 785 | `SP_PUSH_UPGRADE_STATUS` | During firmware update |
| 793 | `SP_PUSH_EXDEV_STATUS` | External-device update |
| 797 | `SP_ERROR_CODE` | On fault |
| 804 | HDMI stream state (`SP_PUSH_HDMI_STREAM_STATE`, 304) | On HDMI change |
| 822 | `SP_OMS_PUSH_UPGRADE_PROGRESS` | During OMS update |

**Command 525 is a real, undocumented Polaris push.** The app suppresses logging
it alongside 518 (POC:70: `if (i != 518 && i != 525)`) yet has **no dispatch case
for it** — the app receives it and silently discards it. The alpaca driver
independently classifies 525 as high-frequency (`POLARIS_POLL_COMMANDS`, ALP:45)
and logs it raw (ALP:760–764), preserving a captured sample:

```
type[2], code[525], val[Tempa509ca361d0000265a ;]
```

The payload begins `Temp` followed by what looks like hex, so **525 is
plausibly a temperature or thermal-telemetry push (inferred)** — but neither
implementation decodes it and the encoding is unresolved. Note the trailing
space before `;`. Also note that 525 is `SP_OMS_VERSION` in the *Theta* constant
table (`application/constant/theta/ThetaCMD.java:72`) — a different meaning in a
different product, so do not import that name.

A client must tolerate 525 arriving at any time. Do not treat an unrecognised
push as a protocol error.

**Command 771 also arrives unsolicited.** The alpaca driver parses it as a file
notification with keys `type`, `class`, `path`, `size`, `cTime`, `duration`
without ever requesting it (ALP:798–802), i.e. the head announces newly written
media. In the app, 771 is the reply to `SP_GET_FILE_LIST`; both uses share the
code.

### Timeouts

The app defines `TIME_OUT_FLAG = "-100000"` (CMD:151) but the socket timeout
itself is set in the OkSocket transport layer, not here. `polaris-mount.c` uses
a 15 s socket timeout (`conn_open(&c, host, port, 15)`) and that has proven
adequate in the field. A goto (519) may legitimately take tens of seconds to
report completion; do not time that out at the socket level.

---

## 5. Command reference

**122 command ids** are documented below — 121 named by the vendor, plus the
unnamed 525 push. The full index is in the [appendix](#appendix-complete-id-index).
Columns:

- **Dir** — `→` app sends, `←` head sends, `↔` both (a sent command is echoed
  back as its own reply).
- **⚠** — marks commands that **move motors** or **change persistent state**.

Almost every command is `↔`: you send it, the head replies with the same code.
The `←`-only entries are pure pushes.

### 5.1 Motion — jog, goto, home

| Code | Name | Type | Dir | Payload | Reply | ⚠ | Cite |
|---|---|---|---|---|---|---|---|
| 513 | `SP_GIMBAL_HADJ_SPEED` | 3 | ↔ | `speed:<int>;` or `x:<int>;y:<int>;` | — | **MOVES** | POC:856, 860 |
| 514 | `SP_GIMBAL_VADJ_SPEED` | 3 | ↔ | `speed:<int>;` or `x:<int>;y:<int>;` | — | **MOVES** | POC:864, 872 |
| 521 | `SP_GIMBAL_RADJ_SPEED` | 3 | ↔ | `speed:<int>;` | — | **MOVES** | POC:876 |
| 515 | `SP_GIMBAL_HADJ_ANGLE` | 3 | ↔ | `angle:<float>;` | — | **MOVES** | POC:906 |
| 516 | `SP_GIMBAL_VADJ_ANGLE` | 3 | ↔ | `angle:<float>;` | — | **MOVES** | POC:914 |
| 522 | `SP_GIMBAL_RADJ_ANGLE` | 3 | ↔ | `angle:<float>;` | — | **MOVES** | POC:918 |
| 532 | `SP_YAW_KEY` | 3 | ↔ | `key:<0\|1>;state:<0\|1\|2>;level:<1..5>;` | — | **MOVES** | POC:922, CMD:144 |
| 533 | `SP_PITCH_KEY` | 3 | ↔ | same | — | **MOVES** | POC:922, CMD:91 |
| 534 | `SP_ROLL_KEY` | 3 | ↔ | same | — | **MOVES** | POC:922, CMD:103 |
| 523 | `SP_GIMBAL_POS_RESET` | 3 | ↔ | `axis:<1\|2\|3>;` | — | **MOVES** | POC:926 |
| 517 | `SP_GET_GIMBAL_POS` | 3 | ↔ | *(empty)* | `yaw:<f>;pitch:<f>;roll:<f>;` — **RADIANS**, see below | — | POC:986, 2537 |
| 535 | `SP_SET_GIMBAL_POS` | 3 | ↔ | `yaw:<f>;pitch:<f>;roll:<f>;` | `ret:` | **MOVES** | POC:990, 2556 |
| 524 | `SP_GIMBAL_EX_AXIS_STA` | 3 | ↔ | *(empty)* | `1;` if third axis present | — | POC:931, 3021 |

**Axis numbering** (523 `axis:`), from the double-tap handler at MAIN:481–497
cross-referenced with the jog handler at MAIN:468–477:

| `axis:` | Axis |
|---|---|
| `1` | Horizontal / yaw / pan |
| `2` | Vertical / pitch / tilt |
| `3` | Roll |

The app sends `axis:1` then `axis:2` as two separate commands to home both
(MAIN:495–496) — there is no "home all" value.

**Key-jog encoding** (532/533/534). The first argument to
`SP_SET_ROCKER_ADJUST(int code, int key, int state, int level)` (POC:922) *is the
command id* — the caller passes 532/533/534 directly
(`polaris/view/ClickRockerView.java:163–187`).

| Field | Values | Source |
|---|---|---|
| `key` | `0` = plus/increase, `1` = minus/decrease | `KEY_PLUS = 0`, `KEY_MINUS = 1` (CMD:18–19) |
| `state` | `0` = released, `1` = pressed, `2` = long-press repeat | `STATE_UP = 0`, `STATE_DOWN = 1`, `STATE_LONG_CLICK = 2` (CMD:146–148) |
| `level` | `1`–`5` speed gear | `MAX_GEAR = 5` (`polaris/view/GearSeekbar.java:14`), default `1` |

**A `state:1` press with no matching `state:0` release leaves the axis moving.**
The app pairs them on touch-down/touch-up (`ClickRockerView.java:205` and `:221`).
Any UI exposing this must guarantee the release — including on page unload,
socket drop, and loss of focus.

**Analog jog `speed:`** (513/514/521) comes from the on-screen analog stick
(MAIN:468–477). The app does not reveal its range, but the alpaca driver's
calibrated implementation does (CTL:983–1006):

| | Slow jog (532/533/534) | Fast jog (513/514/521) |
|---|---|---|
| Payload | `key:<0\|1>;state:<0\|1>;level:<1..5>;` | `speed:<signed int>;` |
| Range | magnitude 1–5, sign carried in `key` | **±2500**, further clipped to magnitude 100–2500 in FAST mode (CTL:896) |
| Repeat | **Latched** — resend only on change (CTL:1030–1033) | **Continuous — must be resent every 50 ms** (CTL:1015, 1023) |

Calibration points from the driver's measured tables (CTL:557–577), axis M1:
raw `5` ≈ **0.208 °/s**, raw `2500` ≈ **8.92 °/s**.

Two details that will bite an implementer:

- **Fast jog is a dead-man command that may have no dead man.** It must be
  re-sent every 50 ms to continue. Whether the head stops on its own when the
  stream stops is **not established by either implementation** — the alpaca
  driver deliberately skips stopping the motors on shutdown
  (CTL:960–967: *"dont bother trying to stop motors as some structures have been
  lost already"*). Verify this on hardware before relying on it. See §9.
- **Level 0 disengages torque.** The driver's PWM logic alternates between
  adjacent non-zero levels and comments `# dont use 0 as it disengages torque`
  (CTL:913). Send `state:0` to stop, not `level:0`.

**Axis-to-motor mapping**, confirmed by the driver's docs (`docs/control.md:466–469`)
and consistent with the app's handlers:

| App name | Command pair (fast / slow) | Motor | Physical axis |
|---|---|---|---|
| HADJ | 513 / 532 | M1 | Azimuth (pan) |
| VADJ | 514 / 533 | M2 | Altitude (tilt) |
| RADJ | 521 / 534 | M3 | Rotation (the astro axis) |

Note the driver's warning that **when M3 is rotated, M1 and M2 no longer
correspond to azimuth and altitude** (`pilot/dist/spa/assets/AnalyseKalman-*.js`).
Motor angles are not simply a rotated alt/az frame.

#### 517 reports RADIANS, and its pitch sign is flipped

This is the easiest unit error in the whole protocol: **519 and 530 take
degrees, 517 returns radians.**

The app passes 517's values through as opaque strings (POC:2537–2552) so it
gives no unit evidence. The alpaca driver converts explicitly (ALP:687):

```python
p_pitch = -rad2deg(float(arg_dict['pitch']))   # note sign switch to align with Alt direction
```

Its comment block (ALP:681–685) is the best raw-hardware documentation that
exists for this command:

| Field | Meaning |
|---|---|
| `yaw` | Axis-1 rotation, radians, **east-positive**, **unwrapped and cumulative** — legitimately reaches ±2π, ±3π |
| `pitch` | Axis-2 rotation, radians, **positive = downward** (hence the negation) |
| `roll` | Axis-3 rotation, radians, cumulative like `yaw` |

Typical park position is `yaw=-0.000280, pitch=0.000267, roll=0.000375` — i.e.
**park is the zero**, and park corresponds to an altitude of about **+47°46′06″**.
The mechanical pitch travel is roughly:

| `pitch` (rad) | Altitude |
|---|---|
| `-0.6144` | highest, +83°00′37″ |
| `0` | park, +47°46′06″ |
| `0.834020` | 0° (horizon) |
| `0.914842` | lowest, −04°38′04″ |

**`yaw` and `roll` do not wrap.** That is what makes cable-wrap detectable — see
[§8](#8-danger-list). It also means 517 is the *only* command that can tell you
how many turns of cable you have accumulated; 518 cannot.

### 5.2 Motion — astro: goto, tracking, alignment

These are the commands `polaris-mount.c` uses, and the ones that matter most.

| Code | Name | Type | Dir | Payload | Reply | ⚠ | Cite |
|---|---|---|---|---|---|---|---|
| 518 | `SP_PUSH_ROTATE_VECTOR` | 3 | ← | — | `w:<f>;x:<f>;y:<f>;z:<f>;compass:<d>;alt:<d>;` | — | POC:2585 |
| 520 | `SP_SET_AHRS_STATE` | **2** | ↔ | `state:<0\|1>;` | `ret:` (`0` = ok) | — | POC:902, 2668 |
| 519 | `SP_SET_GOTO_AU_STATE` | 3 | ↔ | `state:<0\|1>;yaw:<f>;pitch:<f>;lat:<f>;track:<0\|1>;speed:<int>;lng:<f>;` | `ret:`, `track:` | **MOVES** | POC:885, 2639 |
| 531 | `SP_SET_TRACK_AU_STATE` | 3 | ↔ | `state:<0\|1>;speed:<int>;` | `ret:` | **MOVES** | POC:880, 2655 |
| 527 | `SP_SET_YAW` | 3 | ↔ | `compass:<f>;lat:<f>;lng:<f>;` | `ret:` (see §3 caveat) | **PERSISTENT** | POC:890, 2626 |
| 530 | `SP_CALIBRATE_START` | 3 | ↔ | `step:<int>;yaw:<f>;pitch:<f>;lat:<f>;num:<int>;lng:<f>;` | `ret:`, `step:` | **MOVES — see §8** | POC:894, 2569 |
| 536 | `SP_SET_TRACK_HALF_SPEED` | 3 | ↔ | `halfSpeed:<0\|1>;` | — | affects tracking | POC:1119 |

#### 518 — the pose push

The only continuous telemetry. **It does not stream until you enable it** with
520.

```
→  1&520&2&state:1;#          enable
←  518@w:…;x:…;y:…;z:…;compass:…;alt:…;#     … repeatedly
→  1&520&2&state:0;#          disable
```

| Key | Type | Meaning |
|---|---|---|
| `w` `x` `y` `z` | float | Orientation quaternion. The app stores them as `[x, y, z, w]` (POC:2599, 2604, 2609, 2618) — note `w` lands in slot **3**, not 0. |
| `compass` | double | Heading, degrees. **Sign convention unresolved** — see below. |
| `alt` | double | Altitude, degrees, **positive downward** |

**`alt` is negated relative to the altitude you want.** The alpaca driver flips
it on receipt (ALP:712: `p_alt = -float(arg_dict['alt'])`). So 518's `alt` runs
opposite to 519's `pitch`, which is positive-up. Two adjacent commands, two
conventions.

**`compass`'s range is genuinely unknown.** The alpaca driver uses it raw and
only ever compares it with a modulo-360-tolerant helper
(CTL:109–111), so it works whether the value is `270` or `-90` — the driver is
agnostic by construction and its authoritative azimuth comes from the quaternion
instead (CTL:429). Neither source settles this. Derive azimuth from the
quaternion if you need certainty, and see [§9](#9-open-questions).

**Repeated keys.** Only the **first** `x:`/`y:`/`z:`/`w:` occurrence is read by
the app (§2 trap 2), and the alpaca driver's parser explicitly renames repeats to
`w1/x1/y1/z1`, `w2/x2/y2/z2`, … (ALP:639–650) while still reading only the first
(ALP:709). **Both implementations independently confirm the payload carries more
than one quaternion, and neither knows what the later ones are.** See §9.

The head pushes 518 roughly every **200 ms** (`docs/control.md:383`).

The app treats a gap in this stream as a fault: it arms a **5000 ms watchdog**
on every push (MAIN:1527, 1537) and re-sends `520 state:1` if it expires
(MAIN:737). The app also disables AHRS whenever it leaves a mode that needs it
(MAIN:1518, 1549, 2988 and the `onStop` handler at MAIN:437).

AHRS is enabled only for the two direction-dependent modes —
`isNeedDirection(i)` is literally `return i == 7 || i == 8;` (MAIN:280–282).

#### 519 — goto and abort

```
→  1&519&3&state:1;yaw:<Y>;pitch:<P>;lat:<LAT>;track:<T>;speed:<S>;lng:<LNG>;#
```

| Key | Meaning |
|---|---|
| `state` | `1` = go to the target, `0` = **abort the current slew** |
| `yaw` | Target azimuth **in wire encoding** — see below |
| `pitch` | Target altitude, degrees, positive up |
| `lat` / `lng` | Observer latitude / longitude, decimal degrees |
| `track` | `1` = begin sidereal tracking on arrival, `0` = stop at target |
| `speed` | Tracking rate — see the table below |

Abort is the same command with `state:0` and zeroed angles — the app sends
exactly that from its cancel button (MAIN:1629, MAIN:2685), and `polaris-mount.c`
does the same at line 633.

**Tracking rate `speed:`** (shared by 519 and 531):

| `speed` | Rate | Evidence |
|---|---|---|
| `0` | Sidereal (fixed star) | Default `speedType = 0` (`application/database/table/PolarisCamaraParameter.java:103`); UI shows `R.string.state_fixed_star_speed` (`polaris/layout/shootingmodel/StarrySkyLayout.java:922`) |
| `2` | Lunar | Set when the chosen target is the Moon (`polaris/utils/StarrySkyUtils.java:708`); UI shows `R.string.state_moon_speed` and a moon icon (`StarrySkyLayout.java:886, 918`) |
| `1` | **Unknown** — plausibly solar **(inferred)**, never observed in the app |

**Half speed (536).** The sender inverts its own argument (POC:1119):

```java
public boolean SP_SET_TRACK_HALF_SPEED(boolean z) {
    return sendOrder(536, 3, z ? "halfSpeed:0;" : "halfSpeed:1;");
}
```

and the only caller inverts again — `SP_SET_TRACK_HALF_SPEED(!z)` (MAIN:1255).
The two cancel out, so on the wire **`halfSpeed:1` = half-rate tracking enabled**.
Because the double inversion is easy to misread, verify against 284's `halfSpeed`
field rather than assuming.

#### The azimuth encoding — signed, westward-positive

This is the single most important encoding in the protocol and the easiest to get
wrong.

`polaris-mount.c:44–55` records the hardware finding: a 519 carrying `yaw:256` is
answered `ret:-1` and the motors never move, while the same target sent as
`yaw:104` works. **Static analysis of the app independently confirms this and
explains why.**

The app computes the angles it sends in `DrawSkyTools.getSearchTargetSendData`
(SKY:537–563):

```java
float fAcos  = MathUtil.acos(vector3MultiplyMV.y);            // current polar angle
float fAtan2 = MathUtil.atan2(vector3MultiplyMV.z, vector3MultiplyMV.x);
float f  = fAcos  - this.mTargetPhi;                          // Δ polar
float f2 = fAtan2 - this.mTargetTheta;                        // Δ azimuthal
if (f2 >  3.1415927f) f2 -= 6.2831855f;
else if (f2 < -3.1415927f) f2 += 6.2831855f;                  // wrap to (-π, π]
…
searchTargetSendData.levelAngle    = f2 * 57.295776f;         // → yaw
searchTargetSendData.verticalAngle = f  * 57.295776f;         // → pitch
```

`mTargetPhi` / `mTargetTheta` are the target's spherical coordinates in the same
frame (SKY:485–486). Three consequences:

1. **The value is a difference wrapped into (−180, 180], never 0…360.** The
   explicit wrap at SKY:550–554 is the whole reason `yaw:256` is rejected.

2. **The azimuth is negated.** It is computed as `reference − target`, so with
   the reference at the mount's zero heading, `yaw = −azimuth` (mod 360, wrapped).
   Positive `yaw` therefore means *west of zero*. This reproduces
   `polaris-mount.c`'s `az_to_wire()` exactly.

3. **The altitude is NOT negated.** `verticalAngle` derives from
   `acos(y)`, the polar angle from zenith, so
   `Δpolar = (90 − alt_ref) − (90 − alt_target) = alt_target − alt_ref`. With the
   reference level, `pitch = +altitude`. **Only the azimuth flips sign** — an
   asymmetry that has no obvious reason to exist and will silently produce
   mirrored pointing if you assume symmetry.

The conversions:

```
wire_yaw   = wrap180(-azimuth_true)         # azimuth_true: 0..360, N through E
azimuth_true = wrap360(-wire_yaw)
wire_pitch = altitude                        # no transformation
```

where `wrap180` maps into (−180, 180] and `wrap360` into [0, 360).

Equivalent to the C implementation at `polaris-mount.c:51–55`:

```c
static double az_to_wire(double az) {
    az = fmod(az, 360.0);
    if (az < 0) az += 360.0;
    return (az > 180.0) ? (360.0 - az) : -az;
}
```

Spot-check: az 90° (east) → `-90`; az 270° (west) → `+90`; az 200° → `+160`.

**Why the app's own reference is trustworthy.** Before computing, the app pins
the viewer orientation to a fixed quaternion `{0.7071068, 0, 0, 0.7071068}`
(MAIN:370, MAIN:1501) — a canonical level pose — then restores the live sensor
values afterwards (MAIN:386, MAIN:1509). So the angles it sends are measured
against the mount's zero, not against wherever it happens to be pointing.

#### 527 — set compass heading (a THIRD angle convention)

```
→  1&527&3&compass:<deg>;lat:<f>;lng:<f>;#
```

This is how a solved azimuth becomes the mount's alignment, and it is the
approach this repo recommends over 530.

> **`compass` is NOT the 519/530 `yaw` encoding.** It is an unsigned value in
> **[0, 360) offset by 180°**:
>
> ```
> compass = (azimuth_true - 180) mod 360
> ```
>
> So azimuth 0° (North) → `compass:180`; azimuth 180° (South) → `compass:0`.

**Two independent field-tested implementations agree**, which is as strong as
evidence gets here:

```c
/* polaris-mount.c:683-685 — "the mount's compass is 180 deg out from the
   azimuth we reason in" */
fmod(az - 180.0 + 360.0, 360.0)
```

```python
# alpaca driver, ALP:1017
compass = (a_az - 180.0) % 360
```

**Do not reuse one converter for 519 and 527.** 519's `yaw` is signed,
westward-positive, in (−180, 180]; 527's `compass` is unsigned, [0, 360),
offset by 180°. They are different transforms of the same underlying azimuth.

The app itself passes the **phone's** raw heading from its manual calibration
dialog (`polaris/dialog/CalibrationDialog.java:184`):

```java
PolarisOrderCommunication.getInstance().SP_SET_YAW(this.phoneDegress + "", this.lat, this.lng);
```

Whether `phoneDegress` already carries the 180° offset is not determinable from
the decompiled code — but since both hardware-tested clients apply the offset
explicitly, apply it.

**Persistent** — it changes the mount's stored alignment.

#### 530 — the multi-step alignment

```
→  1&530&3&step:<n>;yaw:<f>;pitch:<f>;lat:<f>;num:<int>;lng:<f>;#
```

The step values, recovered from the app's call sites:

| `step` | Meaning | Payload actually used | Cite |
|---|---|---|---|
| `1` | **Enter** alignment mode | all angles `0`, `num:0` | MAIN:2576 (`z2 == true`) |
| `2` | **Submit an alignment star** | `yaw`/`pitch` = target angles (wire encoding), `num` = star index | MAIN:384 |
| `3` | **Leave** alignment mode | all angles `0`, `num:0` | MAIN:2576 (`z2 == false`) |

`num` is the alignment-point index, which implies the head supports **multi-point
alignment** that no client exercises — consistent with the alpaca driver's note
that *"Promised features like three-star alignment never materialized"*
(`docs/control.md:20`). The reply carries `ret:` and `step:` (POC:2569–2581),
though **neither independent client implements a 530 reply handler at all** —
both fire and forget.

> **The three sources disagree about 530, and none of them is obviously wrong.**
> This is the least-settled part of the protocol.

| | step 1 | step 2 | step 3 |
|---|---|---|---|
| **APK** (MAIN:2576, MAIN:384) | zeros, `num:0` — enter align view | real pointing, `num:<index>` | zeros, `num:0` — leave align view |
| **alpaca** (ALP:1030–1034) | zeros, `num:0`, then **sleep 2 s** | real pointing, `num:1`, then sleep 0.2 s | zeros, `num:0` |
| **`polaris-mount.c`** (:697–709) | **real pointing**, `num:1` | real pointing, `num:1` | **not sent at all** |

The APK and the alpaca driver agree on the shape. `polaris-mount.c:699–701`
records the opposite from a hardware capture: *"Captured from the phone app on
real hardware: BOTH steps carry the real pointing, and there is no step:3."*

Possible explanations — none confirmed: a firmware or app version difference, or
the APK's `step:1`/`step:3` zeros coming from the AR-view toggle (a different UI
path) while the actual alignment submit uses a different call site. The two
`SP_CALIBRATE_START` call sites in the app do serve visibly different purposes
(MAIN:2576 toggles the alignment overlay; MAIN:384 submits a star).

The alpaca driver's 2 s and 0.2 s sleeps are hard-coded, asymmetric and
unexplained, with no handshake to replace them.

> **`polaris-mount.c` deliberately does not use 530 at all** — field traces from
> the Aperion work show repeated 530s wedging the motors, and one 527 does the
> job. The alpaca driver also bypasses it entirely when `advanced_alignment` is
> enabled (ALP:526–534). **Two of three implementations avoid this command.**
> See [§8](#8-danger-list).

### 5.3 Motion — limits, levelling, settling

| Code | Name | Type | Dir | Payload | ⚠ | Cite |
|---|---|---|---|---|---|---|
| 541 | `SP_GET_LIMIT_STATE` | 3 | ↔ | *(empty)* → `state:` | — | POC:1240, 3425 |
| 542 | `SP_SET_LIMIT_STATE` | 3 | ↔ | `state:<0\|1>;` | **SAFETY — see §8** | POC:1236 |
| 537 | `SP_GET_TILT_STATE` | 3 | ↔ | *(empty)* → `state:` | — | POC:1204, 3320 |
| 538 | `SP_SET_TILT_STATE` | 3 | ↔ | `state:<0\|1>;` | persistent | POC:1208 |
| 539 | `SP_GET_DITHER_STATE` | 3 | ↔ | *(empty)* → `state:` | — | POC:1212 |
| 540 | `SP_SET_DITHER_STATE` | 3 | ↔ | `state:<0\|1>;` | **MOVES** (dithering nudges between frames) | POC:1216 |
| 543 | `SP_GET_SETTLING_TIME` | 3 | ↔ | *(empty)* → `time:` | — | POC:1244 |
| 544 | `SP_SET_SETTLING_TIME` | 3 | ↔ | `time:<int>;` | persistent | POC:1248 |
| 547 | `SP_GET_AUTO_LEVEL_EN` | 3 | ↔ | *(empty)* → `en:` | — | POC:1376 |
| 548 | `SP_SET_AUTO_LEVEL_EN` | 3 | ↔ | `en:<0\|1>;` | persistent | POC:1380 |
| 549 | `SP_SET_AUTO_LEVEL_STATE` | 3 | ↔ | `state:<0\|1>;` | **MOVES** | POC:1384 |
| 545 | `SP_GET_CAMERA_DIR` | 3 | ↔ | *(empty)* → `dir:` | — | POC:1364 |
| 546 | `SP_SET_CAMERA_DIR` | 3 | ↔ | `dir:<0\|1>;` | **MOVES** (re-orients the camera plate) | POC:1360 |

**542 `state:` polarity is inverted from what you would guess.** The app's switch
is named `openAngleLimitSwitch`, and:

- Turning it **on** requires passing a confirmation dialog first
  (`polaris/dialog/StarHelpDialog.java:179–189` routes an unchecked switch to
  `onOpenAngleLimitSwitchTouch()`, which reveals `dialog_open_angle_limit`
  → `warning_confirm_cancel_dialog`), and the confirm handler sends
  `SP_SET_LIMIT_STATE(1)` (`StarHelpDialog.java:420`).
- Turning it **off** is immediate, no warning, and sends `state:0`
  (`StarHelpDialog.java:185`).

Only the dangerous direction is gated. Therefore:

| `state:` | Meaning |
|---|---|
| `0` | Travel limits **enforced** — the safe default |
| `1` | Travel limits **released** — the head may drive into a hard stop |

`SP_SET_AUTO_LEVEL_STATE(1)` (549) actively levels the head — the app calls it
from MAIN:1022 and cancels with `0` at MAIN:1029.

### 5.4 Mode and program state

| Code | Name | Type | Dir | Payload | ⚠ | Cite |
|---|---|---|---|---|---|---|
| 284 | `SP_PUSH_MODE_STATE` | 2 | ↔ | *(empty)* → see below | — | POC:849, 2383 |
| 285 | `SP_SET_MODE_STATE` | 2 | ↔ | `mode:<int>;` → `mode:`, `ret:` | **changes mode** | POC:852, 2467 |
| 296 | `SP_GET_CONTROL_MODE` | 2 | ↔ | *(empty)* → `mode:` | — | POC:1252 |
| 297 | `SP_SET_CONTROL_MODE` | 2 | ↔ | `mode:<int>;` | persistent | POC:1256 |
| 298 | `SP_GET_EX_TIME` | 2 | ↔ | *(empty)* → `ExTime:` | — | POC:1260 |
| 299 | `SP_SET_EX_TIME` | 2 | ↔ | `ExTime:<int>;` | persistent | POC:1264 |
| 306 | `SP_GET_INTERVAL_TYPE` | 2 | ↔ | *(empty)* | — | POC:1276 |
| 307 | `SP_SET_INTERVAL_TYPE` | 2 | → | `type:<int>` *(no trailing `;`)* | persistent | POC:1268 |

**284 is the mount's status word** and the most useful single query for a UI.
Every key its parser recognises (POC:2408–2440):

| Key | Type | Meaning | Default if absent |
|---|---|---|---|
| `mode` | int | Current shooting mode — table below | `1` |
| `state` | int | Program run state | `0` |
| `track` | string | Astro tracking state — table below | `"3"` |
| `speed` | int | Tracking rate (§5.2) | `0` |
| `halfSpeed` | int | Half-rate flag | `0` |
| `remNum` | int | Frames remaining | `0` |
| `runTime` | int | Elapsed seconds | `0` |
| `remTime` | int | Remaining seconds | `0` |
| `photoNum` | int | Frames shot | `0` |
| `repeNum` | int | Repeat count | `1` |
| `interval` | int | Interval seconds | `0` |
| `startTime` / `endTime` | string | Scheduled window | `"0"` |
| `sun` | flag | **`0` = sunrise, `1` = sunset** — confirmed against the 277 sender (`SunLayout.java:757, 762`) as well as the parser (POC:2434) | false |
| `pause` | flag | `"1"` = paused | false |

`startTime` / `endTime` are formatted `yyyy,MM,dd,HH,mm,ss`
(`SunLayout.java:123`). `remNum` of `-1` means **infinite**
(`StaticLapseLayout.java:269`). **`speed` is parsed and stored (POC:2423, 2454)
but no consumer ever reads it** in this build — do not rely on it.

**`state:` is mode-dependent, not one global enum.** The only universal fact is
that `0` means idle (MAIN:2203, 2233–2237); any non-zero value makes the app
restore its "running" UI. Beyond that the meaning is per-mode:

| Mode | `state` | Meaning | Cite |
|---|---|---|---|
| *any* | `0` | Idle — nothing running | MAIN:2233–2237 |
| 1 PHOTO | `2` | Crowd-removal (289) running | `OrdinaryPhotoLayout.java:178–182` |
| 1 PHOTO | `3` | Burst/continuous shooting | `OrdinaryPhotoLayout.java:183–190` |
| 2 PANO | `1` | Panorama running | MAIN:2196–2198 |
| 3 FOCUS | `1` | Stack capture running (`remNum` valid) | `FocusTrackLayout.java:252–260` |
| 3 FOCUS | `2` | Focus-preview / MF-range phase | `FocusTrackLayout.java:261–273` |
| 7 SUN | `1` | Actively shooting | `SunLayout.java:365–368` |
| 7 SUN | other ≠0 | Scheduled start pending | `SunLayout.java:369–370` |
| 8 ASTRO | `1`, `2` | Astro interval sequence running | `StarrySkyLayout.java:1140–1143` |
| 8 ASTRO | `3` | Sky panorama running | `StarrySkyLayout.java:1138–1139` |
| 9 PROGRAM | `1` | Program running (`remTime` = seconds left) | `PrecompileLayout.java:504–508` |
| 9 PROGRAM | other ≠0 | Appointment pending (`startTime` = scheduled) | `PrecompileLayout.java:509–519` |
| 10 VIDEO | `1` | Recording (`runTime` = elapsed seconds) | `OrdinaryVideoLayout.java:112–117` |

So `state` reads as *"which sub-activity of this mode is active"*, with `0` for
none. **Pause is carried separately** by `pause:`, and **"finished" is never
reported through 284** — completion arrives via each program's own
`recordComplete` step (MAIN:2376–2389).

**`track:` values** — `PolarisCMD` names them (CMD:156, 167, 157, 168) and
`StarrySkyLayout.java:897–903` confirms the UI mapping:

| `track` | Constant | Meaning |
|---|---|---|
| `"0"` | `initStar` | Aligned, **not** tracking — UI offers "start tracking" |
| `"1"` | `trackingStar` | Tracking — UI offers "stop" |
| `"2"` | `pauseStar` | Paused — UI offers "stop" |
| `"3"` | `unAlignStar` | **Not aligned** — tracking controls hidden entirely |

**`mode:` values.** Confirmed from the app: **`7` = Sun**, **`8` = Starry Sky
(astro)**. `isNeedDirection(int i) { return i == 7 || i == 8; }` (MAIN:280–282)
is what gates AHRS, and the astro branch at MAIN:1538 checks `i == 8` before
driving `StarrySkyLayout`. This agrees with `polaris-mount.c`'s "8 = Astro" and
with ASTRO.md's captured `{"mode":8,"track":1,"aligned":true}`. The full mode
enumeration is in §6.

### 5.5 Camera control

| Code | Name | Type | Dir | Payload | Reply keys | Cite |
|---|---|---|---|---|---|---|
| 258 | `SP_SET_ISO` | 1 | ↔ | `iso:<idx>;` | `iso:`, `ret:` | POC:571, 120 |
| 259 | `SP_SET_WB` | 1 | ↔ | `wb:<idx>;` | `wb:`, `ret:` | POC:591, 149 |
| 260 | `SP_SET_EV` | 1 | ↔ | `ev:<idx>;` | `ev:`, `ret:` | POC:587 |
| 261 | `SP_SET_SHUTTER` | 1 | ↔ | `s:<idx>;` | `s:`, `ret:` | POC:575 |
| 276 | `SP_SET_FNUM` | 1 | ↔ | `fNum:<idx>;` | `fNum:`, `ret:` | POC:623, 176 |
| 262 | `SP_SET_FOCUS` | 1 | ↔ | `mod:<m>;f:<v>;` | `ret:` | POC:579 |
| 311 | `SP_SET_FOCUS_ADJ` | 1 | ↔ | `mode:<m>;adj:<v>;` | — | POC:583 |
| 265 | `SP_GET_ISO_INFO` | 1 | ↔ | *(empty)* | option list + index | POC:603 |
| 266 | `SP_GET_WB_INFO` | 1 | ↔ | *(empty)* | option list + index | POC:607 |
| 267 | `SP_GET_EV_INFO` | 1 | ↔ | *(empty)* | option list + index | POC:611 |
| 268 | `SP_GET_SHUTTER_INFO` | 1 | ↔ | *(empty)* | option list + index | POC:615 |
| 275 | `SP_GET_FNUM_INFO` | 1 | ↔ | *(empty)* | option list + index | POC:619 |
| 286 | `SP_CAMERA_INFO` | **4** | ↔ | *(empty)* | camera identity | POC:1079 |
| 282 | `SP_GET_IMG_FORMAT` | **4** | ↔ | *(empty)* | image format | POC:1232 |
| 263 | `SP_SET_VIDEO_RECORD_STATUS` | 2 | ↔ | `state:<0\|1>;` | `ret:` | POC:599 |
| 264 | `SP_SET_PHOTO_RECORD_STATUS` | 2 | ↔ | `state:<s>;bulb:<b>;c:<c>;` | `ret:` | POC:595 |
| 291 | `SP_SET_CAMERA_PREVIEW` | 2 | ↔ | `state:<0\|1>;` | `ret:` | POC:1124 |
| 292 | `SP_GET_CAMERA_PREVIEW` | 2 | ↔ | *(empty)* | preview state | POC:1128 |

**Exposure values are indices, not physical values.** The parsers write the
returned integer into `CameraInfoModel.setSelectIndex(...)` (POC:137, 166) — the
index selects from the option list the matching `*_INFO` query returns. You must
fetch the list (265/266/267/268/275) before you can set anything meaningfully.
The app refreshes all five together in `getCanmeraInfo()` (POC:1465–1473),
rate-limited to once per 2 s.

**263/264 fire the shutter.** They are not motion commands but they do commit
frames to the card.

### 5.6 Creative programs

All are `type:2` and all use a `step:<n>;` sub-command selector. The step
meanings below come from the app's own method names at the cited lines.

| Code | Name | Steps | Cite |
|---|---|---|---|
| 270 | `SP_FOCUS_STACK` | 1–10 | POC:647–689 |
| 271 | `SP_PANORAMIC` | 1–13 | POC:705–741, 1168–1192 |
| 272 | `SP_DELAY_SHOT` (timelapse) | 1–11 | POC:785–845 |
| 277 | `SP_SUN_SHOT` | 1–5 | POC:627–643 |
| 280 | `SP_HDR` | 1–4 | POC:769–781 |
| 283 | `SP_PLC` (motion path recording) | 1–6 | POC:745–765 |
| 289 | `SP_REMOVE_PEOPLE_SHOT` | 1–5 | POC:801–817 |
| 305 | `SP_HOLY_GRAIL` (day-to-night ramping) | 1–13 | POC:1272–1356 |

**270 — focus stacking** (POC:647–689):

| step | Method | Payload |
|---|---|---|
| 1 | `SP_FOCUS_STACK_MF_SET_START` | — |
| 2 | `SP_FOCUS_STACK_MF_SET_END` | — |
| 3 | `SP_FOCUS_STACK_START` | + caller-built params |
| 4 | `SP_FOCUS_STACK_STOP` | — |
| 5 | `SP_FOCUS_STACK_COMPLETION` | — |
| 6 | `SP_FOCUS_STACK_CANCLE` | — |
| 7 | `SP_FOCUS_STACK_RUNNING_INFO` | — |
| 8 | `SP_FOCUS_STACK_START_PREVIEW` | + params |
| 9 | `SP_FOCUS_STACK_PREVIEW_END` | — |
| 10 | `SP_FOCUS_STACK_PREVIEW_CANCLE` | — |

**271 — panorama** (POC:705–741, 1168–1192). **Moves motors** from step 1.

| step | Method | Payload |
|---|---|---|
| 2 | `SP_PANORAMIC_START` | full decode below |
| 3 | `SP_PANORAMIC_COMPLETION_NUMBER` | reply `ret:` carries the remaining count, falling back to `num:` (POC:1836–1848) |
| 4 | `SP_PANORAMIC_END` | — |
| 5 | `SP_PANORAMIC_COMPLETION` | — |
| 6 | `SP_PANORAMIC_CANCLE` | — |
| 7 | `SP_PANORAMA_ROTATE_ANGLE` | `para:<f>,<f>;` |
| 8 | `SP_PANORAMA_START_POINT` | `gimbal:<y>,<p>,<r>;` |
| 9 | `SP_PANORAMA_CANCEL_SELECT_POINT` | — |
| 10 | `SP_PANORAMA_SET_BACK_START_POINT` | `state:<int>;` |
| 11 | `SP_PANORAMA_GET_BACK_START_POINT_STATE` | — |
| 12 | `SP_PANORAMA_PAUSE_SHOOT` | `pause:<0\|1>;` |
| 13 | `SP_PANORAMA_INTERVAL_SHOOT` | `interval:<0..20>;` seconds, clamped by the UI |

**271 step 2 — the full start payload.** Assembled across POC:692–706 and
POC:708–726 plus the pano parameter layouts:

```
1&271&2&step:2;para:<hNum>,<vNum>,<startDir>,<paMode>,<hAngle>,<vAngle>;
        [gimbal:<x>,<y>,<z>;]isp:<0|1>;bgsem:<0|15>;num:<N>;bulb:<sec>;[dir:<0|1>;]#
```

| Field | Meaning |
|---|---|
| `para[0]` `hNum` | **Columns** — horizontal shot count |
| `para[1]` `vNum` | **Rows** — vertical shot count |
| `para[2]` `startDir` | Start corner: `0` centre, `1` up-left, `2` up-right, `3` down-left, `4` down-right (`polaris/layout/PanoSkyNorParamterLayout.java:211–268`) |
| `para[3]` `paMode` | `0` normal grid, `1` "pro" / two-corner, `2` 720° spherical (`PanoramaLayout.java:622, 628`) |
| `para[4]` `hAngle` | **Degrees between adjacent columns**, float. Total sweep = `hAngle × hNum` |
| `para[5]` `vAngle` | **Degrees between adjacent rows**, float. Options 0, 0.1, 0.2, 0.4, 0.6, 0.8, then integers |
| `gimbal` | Start pose `<pan>,<tilt>,<roll>` in **radians** — pro/720 variants only |
| `isp` | Auto-synthesis (in-head stitching) flag |
| `bgsem` | Only ever `0` or `15`. **Meaning undetermined** — no other reference exists in the app |
| `num` | Total shot count (`hNum × vNum × perPointCount`) |
| `bulb` | Bulb exposure seconds, or `0` |
| `dir` | Appended conditionally; **meaning undetermined**, plausibly sweep direction **(inferred)** |

The method that binds the boolean arguments (`MainActivity.clickStart`)
**failed to decompile** (MAIN:769–774 throws
`UnsupportedOperationException("Method not decompiled")`), so the payload
*shape* is certain but the runtime binding of `isp`/`bgsem`/`dir` is not.

**272 — timelapse / motion timelapse** (POC:785–845). **Moves motors.**

| step | Method | Payload |
|---|---|---|
| 1 | `SP_DELAY_SHOT_START` | — |
| 2 | `SP_DELAY_SHOT_SEND_POINT` | caller-built waypoint |
| 3 | `SP_DELAY_SHOT_SEND_END` | `point:<n>;time:<s>;photoCnt:<n>;;preview:<n>;` |
| 4 | `SP_DELAY_SHOT_SHOOTING_NUM` | — |
| 5 | `SP_DELAY_SHOT_SHOOTING_COMPLETE` | — |
| 6 | `SP_DELAY_SHOT_PROCESS_COMPLETE` | — |
| 7 | `SP_DELAY_SHOT_CANCEL` | — |
| 8 | `SP_DELAY_SHOT_END_BACK_SETTING` | `state:<int>;` |
| 9 | `SP_DELAY_SHOT_GET_END_BACK_SETTING_STATE` | — |
| 10 | `SP_DELAY_SHOT_REQUEST_GRAIL_MODE_STATE` | — |
| 11 | `SP_DELAY_SHOT_SET_GRAIL_MODE_STATE` | `state:;bright:;iso:<a>,<b>,<c>;f:<a>,<b>,<c>;s:<n>;` |

Step 3's double semicolon is genuinely emitted — see §2 trap 3. Its arguments
(MAIN:1206–1212): `point` = number of keyframes sent, `time` = total duration in
seconds (**`-1` = infinite**), `photoCnt` = total frames (**`-1` = infinite**),
`preview` = 1 only while previewing a path-lapse.

**Step 2 has three payload shapes**, depending on the mode driving it:

| Mode | Payload |
|---|---|
| 4 static timelapse | `step:2;point:1;time:0;para:<intervalSec float>,<picCount>;bulb:<sec>;` (`StaticLapseLayout.java:239–245`) |
| 8 astro lapse | same shape, integer interval (`StarrySkyLayout.java:1273–1277`) |
| 5 path-lapse | `step:2;point:<i+1>;time:<cumulativeSec>;gimbal:<x>,<y>,<z>;para:<intervalSec>,<picCount>;bulb:0;` (`DynamicLapseLayout.java:605–634`) |

`picCount` of `-1` means infinite. In path-lapse, `time` is the **cumulative**
sum of `interval × picCount` over all prior segments, and `gimbal` is a pose in
**radians**.

Steps 10 and 11 are a **superseded** holy-grail path — the shipping UI uses
command **305** instead (MAIN:2926–2934 is their only caller).

**277 — sun shot**: 1 start (caller-built), 2 `CANCLE`, 3 `END`, 4 `COMPLETE`,
5 `APPOINTMENT_END` (POC:627–643).

**280 — HDR**: 1 `START` (`isp:<0|1>;` + params), 2 `END`, 3 `COMPLETE`,
4 `CANCLE` (POC:769–781).

**283 — `SP_PLC` = the FREE PROGRAM mode (mode 9). MOVES MOTORS.**
Steps (POC:745–765, replies POC:2314–2381 with keys `step:`, `ret:`):
1 `START`, 2 `SEND_PARAMETER` (reply `ret:` = next keyframe index),
3 `END_POINT` (`item1:<count>,<lastTime>;item2:…;item3:…;`),
4 `SET_APPOINTMENT_TIME` (`time:<t>;`), 5 `RUNTIME`, 6 `CANCLE`.

It uploads a **keyframe timeline across three independent tracks**, then replays
it — which is why it drives the head. Step 2 has three payload shapes
(`polaris/layout/shootingmodel/PrecompileLayout.java:951, 966, 981`):

| `item:` | Track | Payload |
|---|---|---|
| `1` | Photo events | `step:2;item:1;point:<n>;time:<sec>;para:<count>,<intervalSec>;` |
| `2` | Camera parameters | `step:2;item:2;point:<n>;time:<sec>;para:<bulbSec>,<sIdx>,<fIdx>,<evIdx>,<isoIdx>,<wbIdx>;mode:<0\|1>;` |
| `3` | **Motion pose** | `step:2;item:3;point:<n>;time:<sec>;para:<pan>,<tilt>,<roll>;mode:<0\|1>;` — **radians** |

`mode:` on tracks 2 and 3 is the interpolation flag: the app's boolean is
`isLineModel` and it sends `(!isLineModel ? 1 : 0)`, so **`mode:0` = linear
interpolation between keyframes, `mode:1` = hold/step**
(`PrecompileAdjustLayout.java:212–258`) — the naming is inferred, the polarity
is not.

Step 5's reply `ret:` feeds `appointmentStateStart` (MAIN:2390–2409):
`0` = appointment ended, `-1` = **failed**, `2` = appointment started.

The `record*` string constants at CMD:158–166 are the app's internal event names
for this and the other program commands.

**289 — `SP_REMOVE_PEOPLE_SHOT`** (runs inside mode 1): 1 `START`
(`bulb:<sec>;num:<shots>;`), 2 `NUM` (reply `ret:` = remaining; the app
auto-resends step 2 to poll), 3 `END`, 4 `COMPLITE`, 5 `CANCLE` (POC:801–817,
replies POC:2188–2262). Multi-frame stack to remove passers-by **(inferred from
the name)**.

> **Vendor bug — the cancel ack is dropped.** The app *sends* cancel as
> `step:5;` (POC:816) but its reply switch has **no case for `"5"`**; only
> `"6"` maps to the cancel event (POC:2222–2228, 2251–2255), and `'5'` falls
> through to `default: b = -1; return`. Either the firmware acks a cancel with
> `step:6`, or cancel acks are silently discarded in the app. Do not assume a
> `step:5` reply will arrive; verify cancellation via 284 instead.

**305 — `SP_HOLY_GRAIL`** (POC:1272–1356), the day-to-night exposure ramp:

| step | Method | Payload |
|---|---|---|
| 1 | `SET_GRAIL_MODEL` | `state:<0\|1>;` |
| 2 | `GET_GRAIL_MODEL` | — |
| 3 | `SET_PRIORITY` | `priority:<a>,<b>,<c>;` |
| 4 | `GET_PRIORITY` | — |
| 5 | `SET_ISO` | `state:<0\|1>;iso:<lo>,<hi>;` |
| 6 | `GET_ISO` | — |
| 7 | `SET_F` | `state:<0\|1>;f:<lo>,<hi>;` (`%.1f`) |
| 8 | `GET_F` | — |
| 9 | `SET_SHUTTER` | `state:<0\|1>;s:<a>,<b>,<c>,<d>;` |
| 10 | `GET_SHUTTER` | — |
| 12 | `GET_BRIGHTNESS` | — |
| 13 | `GET_BRIGHTNESS_RUNTIME` | — |

Step 11 sends a brightness curve built from a `HolyBrokenLine2.MyPoint` list —
`…;nodeCnt:<n>;para:<Δmin>/<value>,<Δmin>/<value>,…;` (POC:1295–1308).

### 5.7 Status and telemetry

| Code | Name | Type | Dir | Reply keys | Cite |
|---|---|---|---|---|---|
| 778 | `SP_GET_BAT_STATE` | 2 | ↔ | `capacity:<int>;charge:<int>;` | POC:1030, 2885 |
| 779 | `SP_PUSH_BAT_STATE` | 2 | ← | same — shares 778's parser | POC:178 |
| 823 | `SP_OMS_BAT_STATE` | 2 | ↔ | accessory battery | POC:1436 |
| 775 | `SP_GET_SD_INFO` | 2 | ↔ | `status:;totalspace:;freespace:;usespace:;` | POC:1026, 2726 |
| 776 | `SP_PUSH_SD_INFO` | 2 | ← | same | POC:2806 |
| 777 | `SP_PUSH_SD_HINT_ID` | 2 | ← | SD warning id | POC:107 |
| 780 | `SP_GET_DEVICE_VERSION` | 2 | ↔ | `hw:;sw:;exAxis:;sv:;` | POC:1034, 2948 |
| 781 | `SP_GET_SYSTEM_TIME` | 2 | ↔ | date/time | POC:1038 |
| 797 | `SP_ERROR_CODE` | 2 | ← | `errorCode:<int>;` | POC:3169 |
| 799 | `SP_GET_CELLULAR_STATE` | 2 | ↔ | modem state | POC:899 |
| 824 | `SP_OMS_RUN_STATE` | 2 | ↔ | accessory run state | POC:1428 |

Battery fields map to `BatteryModel` — `capacity` (percent) and `charge`
(charging flag) (`application/bean/BatteryModel.java`).

**SD `status:` enumeration**, from `application/bean/MemoryModel.java:7–13`:

| Value | Constant |
|---|---|
| `0` | `SDCardNotMounted` |
| `1` | `SDCardMounted` |
| `2` | `SDCardErroNeedFormat` |
| `3` | `SDCardPrepareComplete` |
| `4` | `SDCardFull` |
| `5` | `SDCardPopup` |
| `6` | `SDCardNotExit` |

`totalspace` / `freespace` / `usespace` are `long` (bytes).

### 5.8 Files and media

| Code | Name | Type | Dir | Payload | ⚠ | Cite |
|---|---|---|---|---|---|---|
| 770 | `SP_GET_FILE_COUNT` | 2 | ↔ | *(empty)* | — | POC:994 |
| 771 | `SP_GET_FILE_LIST` | 2 | ↔ | caller-built | — | POC:998 |
| 772 | `SP_DEL_FILE` | 2 | ↔ | caller-built | **DELETES** | POC:1002 |
| 773 | `SP_ADD_FILE` | 2 | ← | — | — | POC:95 |
| 774 | `SP_SD_FORMAT` | 2 | ↔ | *(empty)* | **ERASES CARD** | POC:1022 |
| 786 | `SP_GET_CLASS_FILE_COUNT` | 2 | ↔ | caller-built | — | POC:1006 |
| 787 | `SP_DEL_CLASS` | 2 | ↔ | caller-built | **DELETES** | POC:1010 |
| 788 | `SP_APP_ADD_FILE` | 2 | ↔ | `type:;path:;appTime:` | — | POC:1014 |
| 796 | `SP_GET_ISP_CFG_FILE` | 2 | ↔ | caller-built | — | POC:1018 |
| 798 | `SP_GET_LOG_LIST` | 2 | ↔ | `step:5;` | — | POC:1099 |

**File-type constants** for 771/786/788 (CMD:8–15):

| Value | Type |
|---|---|
| `0` | All |
| `1` | Normal |
| `2` | Timelapse |
| `3` | Focus stack |
| `4` | Panorama |
| `5` | Sun |
| `6` | HDR |
| `7` | Star-sky stack |

### 5.9 System, network, power, firmware

| Code | Name | Type | Dir | Payload | ⚠ | Cite |
|---|---|---|---|---|---|---|
| 782 | `SP_SET_SYSTEM_TIME` | 2 | ↔ | `date:;time:;zone:;` | **PERSISTENT** | POC:1051 |
| 302 | `SP_REBOOT_CONFIRM_MODE` | 2 | ↔ | `confirm:<0\|1>;` | **REBOOTS** | POC:1396 |
| 790 | `SP_APP_PASSWORD_INFO` | 2 | ↔ | `step:1\|2\|5;…` | **PERSISTENT** | POC:1083–1095, 3042 |
| 802 | `SP_GET_WIFI_BAND` | 2 | ↔ | *(empty)* | — | POC:1103 |
| 803 | `SP_SET_WIFI_BAND` | 2 | ↔ | `band:<0\|1>;` | **DROPS LINK** | POC:1108 |
| 804 | `SP_GET_WARNING_TONE_STATE` | 2 | ↔ | *(empty)* | — | POC:1112 |
| 805 | `SP_SET_WARNING_TONE_STATE` | 2 | ↔ | caller-built | persistent | POC:1116 |
| 808 | `SP_SOCKET_CLIENT_TYPE` | 2 | ↔ | `type:<0\|1>;` | — | POC:1131 |
| 815 | `SP_GET_AUTO_OFF_SW` | 2 | ↔ | *(empty)* | — | POC:1368 |
| 816 | `SP_SET_AUTO_OFF_SW` | 2 | ↔ | `sw:<int>;` | **PERSISTENT** | POC:1372 |
| 526 | `SP_TEST` | **2** | ↔ | `step:1..8;…` | **FACTORY — see §8** | POC:1136–1164 |
| 300 | `SP_GET_HDMI_SUPPORT` | 2 | ↔ | *(empty)* | — | POC:1388 |
| 301 | `SP_SET_HDMI_STATE` | 2 | ↔ | `state:<int>;` | persistent | POC:1392 |
| 303 | `SP_GET_HDMI_STATE` | 2 | ↔ | *(empty)* | — | POC:1400 |
| 304 | `SP_PUSH_HDMI_STREAM_STATE` | 2 | ↔ | *(empty)* | — | POC:1404 |

**Cellular (4G accessory):**

| Code | Name | Payload | Cite |
|---|---|---|---|
| 809 | `SP_SET_CELLULAR_APN` | `opt:;apn:;username:;passwd:;auth:;` | POC:1196 |
| 811 | `SP_GET_CELLULAR_IMSI` | *(empty)* | POC:1200 |
| 812 | `SP_GET_CELLULAR_IMEI` | *(empty)* | POC:1220 |
| 813 | `SP_SET_CELLULAR_COMUSB` | `usbmode:<int>;` | POC:1228 |
| 814 | `SP_GET_CELLULAR_HV` | *(empty)* | POC:1224 |

**Firmware update — all of these write flash:**

| Code | Name | Payload | Cite |
|---|---|---|---|
| 783 | `SP_SET_UPGRADE_START` | *(empty)* | POC:1055 |
| 784 | `SP_LOAD_UPGRADE_FW_STATE` | caller-built | POC:1059 |
| 785 | `SP_PUSH_UPGRADE_STATUS` | `state:<int>;` | POC:1063 |
| 791 | `SP_EXDEV_UPGRADE_START` | `devId:<int>;` | POC:1067 |
| 792 | `SP_LOAD_EXDEV_FW_STATE` | `devId:;state:;` | POC:1071 |
| 793 | `SP_PUSH_EXDEV_STATUS` | `devId:;state:;` | POC:1075 |
| 817 | `SP_OMS_ADD` | `state:<0\|1>;` | POC:1408 |
| 818 | `SP_OMS_VERSION` | *(empty)* | POC:1412 |
| 819 | `SP_OMS_UPGRADE_START` | *(empty)* | POC:1416 |
| 820 | `SP_OMS_LOAD_UPGRADE_FW_STATE` | `state:<int>;` | POC:1420 |
| 821 | `SP_OMS_PUSH_UPGRADE_STATUS` | `state:<int>;` | POC:1424 |
| 822 | `SP_OMS_PUSH_UPGRADE_PROGRESS` | ← push | POC:209 |
| 825 | `SP_UPGRADE_RESULT_EXIT` | *(empty)* | POC:1432 |

#### The device password is Base64, not encryption

`SP_APP_PASSWORD_INFO` (790) steps: `1` = get, `2` = set, `5` = reset
(POC:1083, 1091, 1095). The set payload is:

```
step:2;password:<B64>;securityQ:<plain>;securityA:<B64>;
```

`UtilFunction.encryptPassword` (`application/utils/UtilFunction.java:386–395`)
is **Base64 and nothing else**:

```java
String strReplace = Base64.encodeToString(str.getBytes("UTF-8"), 0).replace("\n", "");
```

The default is `DEFAULT_PASSWORD = "0000"` (CMD:6). Treat the device password as
plaintext on the wire; it is obfuscation, not protection. It does **not** gate
the TCP socket — see §4.

---

## 6. Mode enumeration

**The enumeration is settled.** The labels below are the vendor's own, resolved
from `resources.arsc` inside the APK via the resource ids referenced in
`polaris/R.java`, so they are not inferred.

| `mode` | Vendor label | Program command | UI class |
|---|---|---|---|
| `1` | PHOTO | 264, or 289 for crowd-removal | `OrdinaryPhotoLayout` |
| `2` | PANO | 271 | `PanoramaLayout` |
| `3` | FOCUS STACK | 270 | `FocusTrackLayout` |
| `4` | TIMELAPSE (static) | 272 | `StaticLapseLayout` |
| `5` | PATH-LAPSE (motion timelapse) | **272** | `DynamicLapseLayout` |
| `6` | HDR | 280 | `HDRLayout` |
| `7` | **SUN** | 277 | `SunLayout` |
| `8` | **ASTRO** | 519 / 531 / 530, plus 271 or 272 | `StarrySkyLayout` |
| `9` | FREE PROGRAM | **283 `SP_PLC`** | `PrecompileLayout` |
| `10` | VIDEO | 263 | `OrdinaryVideoLayout` |

The mode integer is the fourth constructor argument of each `ShootingModel`
in `polaris/dialog/SelectShootingModelDialog.java:147–156`, and it is what
`SP_SET_MODE_STATE(i)` receives (MAIN:2746–2757 → POC:852).

Corroborated four ways: (1) the resource labels above; (2) the cancel-dispatch
switch `clickStop()` at MAIN:802–864, which maps each mode to its program's
cancel command; (3) **MAIN:2349**, a single condition enumerating every valid
`(replyCode, mode)` pair — `(264,1) (289,1) (280,6) (264,6) (271,2) (271,8)
(270,3) (277,7) (283,9) (272,4) (272,5) (272,8) (263,10)`, the cleanest
command↔mode map in the app; (4) the alpaca driver's independent declaration
(ALP:81) and the Pilot UI bundle, which list the same ten.

**No mode `0` and no mode `11` exists** — `RightControlLayout.java:191–224`
switches over 1–10 only. The default is `1` (MAIN:235). The driver enforces the
send range as 1–10 (ALP:1170).

`isNeedDirection(int i) { return i == 7 || i == 8; }` (MAIN:280–282) — only SUN
and ASTRO enable AHRS. ASTRO.md's captured `{"mode":8,"track":1,"aligned":true}`
agrees, as does `polaris-mount.c`.

> **`SP_PLC` is mode 9, not mode 5.** "PLC" is the vendor's abbreviation for the
> **FREE PROGRAM** mode — a pre-programmed keyframe timeline, not the
> path-lapse. Mode 5 (PATH-LAPSE) drives command **272**, the same command as
> static timelapse. The `record*` string constants at CMD:158–166 belong to 283.

**Mode 8 has three sub-modes, and they are not carried in `mode:`.** The app
keeps `skyModeSelectType` locally (`StarrySkyLayout.java:1144–1157`):

| `skyModeSelectType` | Behaviour | Command |
|---|---|---|
| `0` | Astro interval / night-exposure sequence | **272** |
| `1` | Sky panorama, normal | **271** |
| `2` | Sky panorama, "pro" | **271** |

That is why mode 8 appears against both 271 and 272 at MAIN:2349. A client
cannot learn the sub-mode from the wire — it must track it itself. Mode 2 has a
parallel `panoSelectType` (normal / pro / 720).

**Mode switching is blocked while tracking.** MAIN:2751 refuses a mode change
when the mode is 8 and `track` is `"1"` or `"2"`. Stop tracking first.

---

## 7. Sequencing and state machine

### Minimum viable session

```
connect 192.168.0.1:9090
→ 1&808&2&type:0;#           register as a client        (else radio sleeps in 60 s)
→ 1&284&2&#                  read mode / track / state
```

### Reading pose

```
→ 1&520&2&state:1;#          enable AHRS
← 518@w:…;x:…;y:…;z:…;compass:…;alt:…;#     repeats until disabled
   … re-send 520 state:1 if >5 s passes with no push (the app's watchdog)
→ 1&520&2&state:0;#          disable when done
```

`517` is the alternative: a one-shot `yaw`/`pitch`/`roll` read of the **motor**
angles, versus 518's fused AHRS attitude. Use 517 for mechanical position, 518
for where the camera actually points.

### Astro: align, goto, track

Angles below use two different conversions — see §5.2. Write them as separate
functions so they cannot be confused:

```
wire_yaw     = wrap180(-azimuth)        # for 519 and 530, signed, west-positive
wire_compass = (azimuth - 180) mod 360  # for 527 only, unsigned
wire_pitch   = altitude                 # no transformation
```

```
0.  → 1&285&2&mode:8;#               enter ASTRO   (blocked while track is 1 or 2)
1.  → 1&284&2&#                      check track: — "3" means UNALIGNED
2.  align:
      preferred:  → 1&527&3&compass:<wire_compass>;lat:<f>;lng:<f>;#
      the app's:  → 1&530&3&step:1;…#   enter        ← three sources disagree,
                  → 1&530&3&step:2;yaw:<wire_yaw>;pitch:<alt>;…;num:<i>;#   §5.2
                  → 1&530&3&step:3;…#   leave
3.  → 1&284&2&#                      track: should now be "0" (aligned, idle)
4.  → 1&519&3&state:1;yaw:<wire_yaw>;pitch:<alt>;lat:;track:1;speed:0;lng:;#
        ← consume TWO replies: ret:1 (starting) then ret:2 (stopping)
5.  poll 1&284&2&# until track reports arrival
6.  → 1&531&3&state:1;speed:0;#      tracking on   (if track:0 was used in step 4)
    → 1&531&3&state:0;speed:0;#      tracking off
abort a slew at any time:
    → 1&519&3&state:0;yaw:0.0;pitch:0.0;lat:<f>;track:0;speed:0;lng:<f>;#
```

**Ordering rules that matter:**

- **Enter mode 8 first**, and note you **cannot leave it while tracking** —
  MAIN:2751 refuses a mode change when `track` is `"1"` or `"2"`. Send
  `531 state:0` before switching modes.
- **Align before goto.** With `track:"3"` the app hides the tracking controls
  entirely (`StarrySkyLayout.java:905–909`). A goto issued while unaligned points
  at the wrong sky.
- **A goto returns two replies** (`ret:1` then `ret:2`, §3). Consume both or the
  reply stream desynchronises.
- **`lat`/`lng` are required on every 519, 527 and 530.** They are not sticky —
  the app re-sends them each time (MAIN:1506, 1629, 2685, 2722).
- **AHRS is mode-scoped.** The app enables 520 on entering mode 7/8 and disables
  it on leaving (MAIN:737–739) and on `onStop` (MAIN:437). Leaving AHRS on
  outside those modes is untested.
- **Mode switching cancels the running program.** The app sends the program's
  own cancel step before switching (MAIN:820–845), then 285.

### Registration and reconnection

Re-send 808 after every reconnect. `polaris-mount.c:380–383` notes that
`SOCKET_CLOSE`/`ClientCtxDel` fire the moment you disconnect, which restarts the
60 s timer.

---

## 8. Danger list

This section gates what a control UI may expose.

### Will physically move the head

| Code | Why |
|---|---|
| 513, 514, 521 | Continuous-speed jog — runs until told to stop |
| 515, 516, 522 | Relative angle move |
| 532, 533, 534 | Key jog — **a `state:1` with no `state:0` runs forever** |
| 519 | Goto |
| 523 | Axis home |
| 530 | Alignment sequence |
| 531 | Tracking |
| 535 | Absolute position set |
| 540 | Dithering (nudges between frames) |
| 546 | Camera-plate re-orientation |
| 549 | Auto-level |
| 271, 272, 283 | Panorama, motion timelapse, path replay — all drive the head unattended |

### Specifically hazardous

**530 — the multi-step alignment.** `polaris-mount.c:19–21` records that repeated
530s were observed **wedging the Polaris' motors** during the Aperion work, and
the tool deliberately uses a single 527 instead. Do not expose 530 in a UI. If it
must exist, gate it behind an explicit "I understand" confirmation and never
issue it in a retry loop.

**542 `state:1` — releases the travel limits.** With limits released the head can
be driven into a mechanical hard stop. The vendor app itself requires a warning
dialog to enable this and no confirmation to disable it
(`StarHelpDialog.java:179–189, 420`). **Never send `state:1` without an explicit,
per-session user confirmation, and prefer never sending it at all.** Note the
inverted polarity documented in §5.3 — `0` is the safe value.

**Altitude band.** `polaris-mount.c:57–63` gates its own gotos to **12°–80°**,
adopted from alpaca-benro-polaris, whose author gates with
`if p_alt>12 and p_alt<80`. Sending a `pitch` outside that band is not known to
be safe. The C tool additionally refuses a single slew larger than `--max-slew`
(default 180°) and refuses to derive an alignment above 65°, because azimuth is
degenerate near the zenith.

**Out-of-range azimuth is rejected, not clamped.** A `yaw` outside (−180, 180]
returns `ret:-1` and the motors do not move (`polaris-mount.c:44–50`). That is a
safe failure, but a UI that ignores `ret` will report a slew that never happened.

**Cable wrap is the failure mode that damages hardware.** From the alpaca
driver's docs (`docs/control.md:305`):

> the mount "can rotate far enough to wrap cables tightly around the tripod,
> risking damage to cables, camera and mount"

This is possible because **517's `yaw` and `roll` do not wrap** — they
accumulate past ±360° (§5.1). That is also what makes it *detectable*: poll 517
and track cumulative rotation. 518 cannot tell you this.

The alpaca driver enforces its limits **entirely client-side** (CTL:1604–1619),
fed only by 517 (ALP:700) — **stop polling 517 and you lose all protection**.
Its defaults (`config.toml:66–71`) are Z1 ±270°, Z2 −55°/+37°, Z3 ±270°, while
its own Pilot UI ships tighter values (±190°, −32°/+40°). **The two disagree**,
so treat neither as the hardware limit.

Note the head *does* have a limit feature of its own (541/542) that the alpaca
driver does not use — which is why it reimplemented one. A client should
probably use both.

`docs/control.md:334` is worth quoting in full for a UI author:

> "Although these safety measures are built into V2.0, we advise against
> operating the Polaris **unattended**, as some failure modes may still be
> beyond the driver's control."

**Fast jog may have no dead-man timeout.** 513/514/521 must be re-sent every
50 ms to sustain motion, but whether the head stops on its own when the stream
stops is **unverified** — the alpaca driver deliberately skips stopping motors
during shutdown (CTL:960–967). A web UI must send an explicit stop on
disconnect, page unload, and loss of focus, and must not rely on the head
noticing that the client went away. See §9.

### Writes flash or persistent state

| Code | Effect |
|---|---|
| 783, 784, 785 | Main firmware update |
| 791, 792, 793 | External-device firmware |
| 819, 820, 821, 825 | OMS accessory firmware |
| 774 | **Formats the SD card** — irreversible |
| 772, 787 | Delete files / classes |
| 782 | Sets the RTC. Note the Polaris clock quirk recorded at `polaris-mount.c:428`: it runs **local** time while reporting itself as UTC |
| 790 | Sets the device password (Base64 only) |
| 816 | Auto-power-off setting |
| 803 | Wi-Fi band — **will drop the link you sent it over** |
| 297, 299, 301, 307, 538, 544, 548, 805 | Assorted persistent settings |
| 302 | Reboot |

### Factory diagnostics — 526 `SP_TEST`

Steps 1–8 (POC:1136–1164) are a factory test harness: `1` cable release
(`A:<i>;B:<i>;`), `2` HDMI test, `3` 4G on/off, `4` **raw AT command passthrough
to the modem** (`step:4;AT:<cmd>;`), `5` UART debug, `6` **system sleep**,
`7` 4G reset, `8` battery warning test.

**No code path in the shipping app calls any of them** — a tree-wide search
finds no caller, and the reply parser posts the raw string with no key parsing
at all (POC:3289–3291), gated behind `AppGlobalDataMgr.isTestMode`. These are
reachable only from a hidden or internal build.

None of this belongs in a user-facing UI. Step 4 hands arbitrary strings to the
cellular modem, and step 6 sleeps the system out from under your own connection.

---

## 9. Open questions

Each names the specific place to trace at runtime.

1. **What does the leading `1&` mean?** It is a fixed literal shared with the
   unrelated Theta product (POC:1443 and
   `theta/singleton/ThetaOrderCommunication.java:619`), and ASTRO.md's firmware
   log shows only `code`/`type`/`val` being extracted, so it is consumed
   earlier. Best guess is a protocol version or an app→device direction marker
   **(inferred, unverified)**. *To resolve:* send a frame with a different
   leading digit and observe whether `rcv msg from App[N]` still parses.

2. **Does fast jog (513/514/521) have a mount-side dead-man timeout?**
   *This is the most safety-relevant unknown here.* The command must be
   re-sent every 50 ms to sustain motion, but no implementation establishes
   what happens when the stream simply stops — the alpaca driver skips stopping
   motors on shutdown entirely (CTL:960–967). *To resolve:* issue a small
   `speed:` with limits enabled, stop sending, and time how long the head keeps
   moving. Until then, always send an explicit stop.

3. **Is 527's `ret` really inverted?** `parseSP_SET_YAW` (POC:2632) tests
   `ret == 1` for success where every other parser tests `ret == 0`. Relatedly,
   **520's success value is contested** — ALP:846 documents `ret:1`, POC:2669
   treats `0` as success. *To resolve:* send each with a deliberately invalid
   argument and compare the returned `ret` against a valid one.

4. **Which 530 sequence is correct?** The APK, the alpaca driver, and
   `polaris-mount.c` describe three different step patterns (§5.2). *To
   resolve:* capture a full alignment from the phone app from the first byte,
   the way ASTRO.md captured the 808 registration. Until then, prefer 527.

5. **What does 525 carry?** It is confirmed to be a real high-rate Polaris push
   (§4), and its payload begins `Temp` followed by hex — but nothing decodes it.
   *To resolve:* log 525 across a temperature change and correlate.

6. **What is in 518 beyond the first quaternion?** Both independent
   implementations detect repeated `w`/`x`/`y`/`z` keys and both read only the
   first (POC:2596–2620, ALP:639–650). Nobody knows how many there are or what
   they mean. *To resolve:* dump one raw 518 frame verbatim.

7. **Is 518's `compass` signed or 0–360?** The alpaca driver is agnostic by
   construction — it compares modulo 360 (CTL:109–111) — and the app feeds it
   straight into a renderer. *To resolve:* point the head due west and read the
   raw value: `270` or `-90` settles it.

8. **`speed:1`** in 519/531 — 0 (sidereal) and 2 (lunar) are confirmed; 1 is
   unobserved and plausibly solar **(inferred)**.

9. **`bgsem:` and `dir:` in command 271.** `bgsem` is only ever `0` or `15` and
   has no other reference in the app; `dir` is appended conditionally by an
   overload whose caller (`MainActivity.clickStart`) **failed to decompile**
   (MAIN:769–774). *To resolve:* capture a panorama start from the app, or
   re-decompile that method with a different tool.

10. **`SP_SET_CONTROL_MODE` (297) values.** Never called with a literal in the
    decompiled tree.

11. **The 797 `errorCode:` table.** The app maps codes to localised strings, so
    the numbers are not in the Java. *To resolve:* read `resources.arsc` from
    the APK — that is how §6's mode labels were recovered — or provoke faults on
    hardware.

12. **Is 808 actually required before other commands are honoured?** It is
    definitely required to keep the radio awake (§4), but the app issues 790,
    799, 296, 300 and 298 *concurrently* with it and never waits for the ack, so
    the negative case is never exercised. *To resolve:* connect, skip 808, and
    try a query.

13. **Is a mismatched `type` field ever rejected?** The alpaca driver uses
    different values from the app for 802 and 824 and works (§2). *To resolve:*
    send a motion command with the wrong `type` and observe.

14. **Whether 271/272/283 respect the 542 travel limits.** Untested. Treat
    unattended program commands as capable of reaching a hard stop if limits
    have been released.

---

## Appendix: complete id index

| Id | Name | Type | Section |
|---|---|---|---|
| 258 | `SP_SET_ISO` | 1 | 5.5 |
| 259 | `SP_SET_WB` | 1 | 5.5 |
| 260 | `SP_SET_EV` | 1 | 5.5 |
| 261 | `SP_SET_SHUTTER` | 1 | 5.5 |
| 262 | `SP_SET_FOCUS` | 1 | 5.5 |
| 263 | `SP_SET_VIDEO_RECORD_STATUS` | 2 | 5.5 |
| 264 | `SP_SET_PHOTO_RECORD_STATUS` | 2 | 5.5 |
| 265 | `SP_GET_ISO_INFO` | 1 | 5.5 |
| 266 | `SP_GET_WB_INFO` | 1 | 5.5 |
| 267 | `SP_GET_EV_INFO` | 1 | 5.5 |
| 268 | `SP_GET_SHUTTER_INFO` | 1 | 5.5 |
| 270 | `SP_FOCUS_STACK` | 2 | 5.6 |
| 271 | `SP_PANORAMIC` | 2 | 5.6 |
| 272 | `SP_DELAY_SHOT` | 2 | 5.6 |
| 275 | `SP_GET_FNUM_INFO` | 1 | 5.5 |
| 276 | `SP_SET_FNUM` | 1 | 5.5 |
| 277 | `SP_SUN_SHOT` | 2 | 5.6 |
| 280 | `SP_HDR` | 2 | 5.6 |
| 282 | `SP_GET_IMG_FORMAT` | 4 | 5.5 |
| 283 | `SP_PLC` | 2 | 5.6 |
| 284 | `SP_PUSH_MODE_STATE` | 2 | 5.4 |
| 285 | `SP_SET_MODE_STATE` | 2 | 5.4 |
| 286 | `SP_CAMERA_INFO` | 4 | 5.5 |
| 289 | `SP_REMOVE_PEOPLE_SHOT` | 2 | 5.6 |
| 291 | `SP_SET_CAMERA_PREVIEW` | 2 | 5.5 |
| 292 | `SP_GET_CAMERA_PREVIEW` | 2 | 5.5 |
| 296 | `SP_GET_CONTROL_MODE` | 2 | 5.4 |
| 297 | `SP_SET_CONTROL_MODE` | 2 | 5.4 |
| 298 | `SP_GET_EX_TIME` | 2 | 5.4 |
| 299 | `SP_SET_EX_TIME` | 2 | 5.4 |
| 300 | `SP_GET_HDMI_SUPPORT` | 2 | 5.9 |
| 301 | `SP_SET_HDMI_STATE` | 2 | 5.9 |
| 302 | `SP_REBOOT_CONFIRM_MODE` | 2 | 5.9 |
| 303 | `SP_GET_HDMI_STATE` | 2 | 5.9 |
| 304 | `SP_PUSH_HDMI_STREAM_STATE` | 2 | 5.9 |
| 305 | `SP_HOLY_GRAIL` | 2 | 5.6 |
| 306 | `SP_GET_INTERVAL_TYPE` | 2 | 5.4 |
| 307 | `SP_SET_INTERVAL_TYPE` | 2 | 5.4 |
| 311 | `SP_SET_FOCUS_ADJ` | 1 | 5.5 |
| 513 | `SP_GIMBAL_HADJ_SPEED` | 3 | 5.1 |
| 514 | `SP_GIMBAL_VADJ_SPEED` | 3 | 5.1 |
| 515 | `SP_GIMBAL_HADJ_ANGLE` | 3 | 5.1 |
| 516 | `SP_GIMBAL_VADJ_ANGLE` | 3 | 5.1 |
| 517 | `SP_GET_GIMBAL_POS` | 3 | 5.1 |
| 518 | `SP_PUSH_ROTATE_VECTOR` | 3 | 5.2 |
| 519 | `SP_SET_GOTO_AU_STATE` | 3 | 5.2 |
| 520 | `SP_SET_AHRS_STATE` | **2** | 5.2 |
| 521 | `SP_GIMBAL_RADJ_SPEED` | 3 | 5.1 |
| 522 | `SP_GIMBAL_RADJ_ANGLE` | 3 | 5.1 |
| 523 | `SP_GIMBAL_POS_RESET` | 3 | 5.1 |
| 524 | `SP_GIMBAL_EX_AXIS_STA` | 3 | 5.1 |
| **525** | *(unnamed — high-rate push, `Temp…`)* | 2 | 4 |
| 526 | `SP_TEST` | **2** | 5.9 |
| 527 | `SP_SET_YAW` | 3 | 5.2 |
| 530 | `SP_CALIBRATE_START` | 3 | 5.2 |
| 531 | `SP_SET_TRACK_AU_STATE` | 3 | 5.2 |
| 532 | `SP_YAW_KEY` | 3 | 5.1 |
| 533 | `SP_PITCH_KEY` | 3 | 5.1 |
| 534 | `SP_ROLL_KEY` | 3 | 5.1 |
| 535 | `SP_SET_GIMBAL_POS` | 3 | 5.1 |
| 536 | `SP_SET_TRACK_HALF_SPEED` | 3 | 5.2 |
| 537 | `SP_GET_TILT_STATE` | 3 | 5.3 |
| 538 | `SP_SET_TILT_STATE` | 3 | 5.3 |
| 539 | `SP_GET_DITHER_STATE` | 3 | 5.3 |
| 540 | `SP_SET_DITHER_STATE` | 3 | 5.3 |
| 541 | `SP_GET_LIMIT_STATE` | 3 | 5.3 |
| 542 | `SP_SET_LIMIT_STATE` | 3 | 5.3 |
| 543 | `SP_GET_SETTLING_TIME` | 3 | 5.3 |
| 544 | `SP_SET_SETTLING_TIME` | 3 | 5.3 |
| 545 | `SP_GET_CAMERA_DIR` | 3 | 5.3 |
| 546 | `SP_SET_CAMERA_DIR` | 3 | 5.3 |
| 547 | `SP_GET_AUTO_LEVEL_EN` | 3 | 5.3 |
| 548 | `SP_SET_AUTO_LEVEL_EN` | 3 | 5.3 |
| 549 | `SP_SET_AUTO_LEVEL_STATE` | 3 | 5.3 |
| 770 | `SP_GET_FILE_COUNT` | 2 | 5.8 |
| 771 | `SP_GET_FILE_LIST` | 2 | 5.8 |
| 772 | `SP_DEL_FILE` | 2 | 5.8 |
| 773 | `SP_ADD_FILE` | 2 | 5.8 |
| 774 | `SP_SD_FORMAT` | 2 | 5.8 |
| 775 | `SP_GET_SD_INFO` | 2 | 5.7 |
| 776 | `SP_PUSH_SD_INFO` | 2 | 5.7 |
| 777 | `SP_PUSH_SD_HINT_ID` | 2 | 5.7 |
| 778 | `SP_GET_BAT_STATE` | 2 | 5.7 |
| 779 | `SP_PUSH_BAT_STATE` | 2 | 5.7 |
| 780 | `SP_GET_DEVICE_VERSION` | 2 | 5.7 |
| 781 | `SP_GET_SYSTEM_TIME` | 2 | 5.7 |
| 782 | `SP_SET_SYSTEM_TIME` | 2 | 5.9 |
| 783 | `SP_SET_UPGRADE_START` | 2 | 5.9 |
| 784 | `SP_LOAD_UPGRADE_FW_STATE` | 2 | 5.9 |
| 785 | `SP_PUSH_UPGRADE_STATUS` | 2 | 5.9 |
| 786 | `SP_GET_CLASS_FILE_COUNT` | 2 | 5.8 |
| 787 | `SP_DEL_CLASS` | 2 | 5.8 |
| 788 | `SP_APP_ADD_FILE` | 2 | 5.8 |
| 790 | `SP_APP_PASSWORD_INFO` | 2 | 5.9 |
| 791 | `SP_EXDEV_UPGRADE_START` | 2 | 5.9 |
| 792 | `SP_LOAD_EXDEV_FW_STATE` | 2 | 5.9 |
| 793 | `SP_PUSH_EXDEV_STATUS` | 2 | 5.9 |
| 796 | `SP_GET_ISP_CFG_FILE` | 2 | 5.8 |
| 797 | `SP_ERROR_CODE` | 2 | 5.7 |
| 798 | `SP_GET_LOG_LIST` | 2 | 5.8 |
| 799 | `SP_GET_CELLULAR_STATE` | 2 | 5.7 |
| 802 | `SP_GET_WIFI_BAND` | 2 | 5.9 |
| 803 | `SP_SET_WIFI_BAND` | 2 | 5.9 |
| 804 | `SP_GET_WARNING_TONE_STATE` | 2 | 5.9 |
| 805 | `SP_SET_WARNING_TONE_STATE` | 2 | 5.9 |
| 808 | `SP_SOCKET_CLIENT_TYPE` | 2 | 4, 5.9 |
| 809 | `SP_SET_CELLULAR_APN` | 2 | 5.9 |
| 811 | `SP_GET_CELLULAR_IMSI` | 2 | 5.9 |
| 812 | `SP_GET_CELLULAR_IMEI` | 2 | 5.9 |
| 813 | `SP_SET_CELLULAR_COMUSB` | 2 | 5.9 |
| 814 | `SP_GET_CELLULAR_HV` | 2 | 5.9 |
| 815 | `SP_GET_AUTO_OFF_SW` | 2 | 5.9 |
| 816 | `SP_SET_AUTO_OFF_SW` | 2 | 5.9 |
| 817 | `SP_OMS_ADD` | 2 | 5.9 |
| 818 | `SP_OMS_VERSION` | 2 | 5.9 |
| 819 | `SP_OMS_UPGRADE_START` | 2 | 5.9 |
| 820 | `SP_OMS_LOAD_UPGRADE_FW_STATE` | 2 | 5.9 |
| 821 | `SP_OMS_PUSH_UPGRADE_STATUS` | 2 | 5.9 |
| 822 | `SP_OMS_PUSH_UPGRADE_PROGRESS` | 2 | 5.9 |
| 823 | `SP_OMS_BAT_STATE` | 2 | 5.7 |
| 824 | `SP_OMS_RUN_STATE` | 2 | 5.7 |
| 825 | `SP_UPGRADE_RESULT_EXIT` | 2 | 5.9 |
