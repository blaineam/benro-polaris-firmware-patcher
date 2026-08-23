# Networking: joining your home network without losing the app

By default the Polaris is an access point and nothing else. Your phone joins
`polaris_xxxxxx`, the Benro app talks to `192.168.0.1:9090`, and that is the
only way in. Working on the device means sitting on its access point, cut off
from everything else — and worse, the firmware powers the radio down 60 s after
the app disconnects, taking ssh with it.

This adds a **second, station-mode interface** alongside the access point. The
AP keeps running exactly as before, so the Benro app is unaffected, while the
device also appears on your home network.

```
wlan0  192.168.0.1     access point   — the Benro app, unchanged
wlan1  10.0.0.110      station        — your home network, ssh + dashboard
```

## The hard constraint: ONE CHANNEL

`iw phy` advertises:

```
valid interface combinations:
  * #{ AP } <= 2, #{ managed } <= 2, ... total <= 4, #channels <= 2
```

**The `#channels <= 2` part is not true on this hardware.** The CYW43455 has a
single radio. Holding the AP on 5 GHz while associating a station on 2.4 GHz
means being in two bands at once, which needs RSDB — two physical radios. The
firmware crashes attempting it and takes the whole device down: no TCP service
answers, ping still works, and it reboots itself shortly after.

That was observed **four times**, always at the moment of association, never
during a scan — scanning is a brief off-channel excursion the chip handles
fine. Once both interfaces were on the same channel, association took **one
second**.

So the setup script scans for your network first, derives its channel, moves the
access point to that channel, and only then associates. **The AP follows the
home network's channel.** If your router is on 2.4 GHz, the Polaris AP moves to
2.4 GHz, which is slower for the app's live-view stream. That is the price of a
single-radio chip; there is no configuration that avoids it.

## Setup

Everything lives in `/app/sd/polaris-wifi/`.

**From the dashboard** — the *Home network* card: enter SSID and password, press
**Save**, then **Join**. Tick **Join automatically at power-up** and it happens
on its own from then on.

**Or over ssh**, if you would rather the password never crossed an HTTP
connection:

```sh
ssh -t root@192.168.0.1 /app/sd/polaris-wifi/setup-wifi.sh
```

Either way only the **hashed PSK** is stored. `wpa_passphrase` echoes the
plaintext back as a `#psk=` comment; that line is stripped. The dashboard path
never puts the password on a command line (argv is world-readable through
`/proc`) — it goes to `wpa_passphrase` on stdin via a `0600` file that is
unlinked immediately.

WPA2-PSK only. WPA3-SAE is deliberately not built: it needs elliptic-curve
crypto the internal implementation lacks, and pulling in OpenSSL would mean
shipping a TLS stack to a device that has none. A **WPA3-only** network will not
associate.

## What runs, and when

| | |
|---|---|
| `polaris-autojoin.sh` | boot-time join; scans, moves the AP's channel, associates, DHCP |
| `polaris-apsta.sh` | the same thing manually (`up` / `down` / `status`) |
| `setup-wifi.sh` | interactive credential entry over ssh |
| `udhcpc.script` | applies the lease (see below) |
| `wpa_supplicant`, `iw`, `wpa_cli` | cross-built; the device ships none of these |

`polaris-autojoin.sh` retries five times with increasing backoff. A router can
refuse an association for reasons that clear on their own — it may have shunned
a client that associated and vanished repeatedly, it may be steering bands, or
at power-up it may not be ready yet. One attempt and give up would turn any of
those into "auto-join is broken".

**Failure is always safe.** Anything incomplete restores the AP's original
configuration and removes the station interface. The worst case is "no home
network", never "no device" — which matters when the radio being reconfigured is
the one carrying your ssh session.

## Two traps worth knowing

**`udhcpc -s` is not optional.** BusyBox `udhcpc` configures nothing itself and
this device ships no default script. Without one it obtains the lease and
silently discards it, logging `lease of 10.0.0.110 obtained` while the interface
stays address-less. That is indistinguishable from a DHCP failure unless you
look at the interface.

**`ASSOC-REJECT status_code=1` is the router refusing you, not a bad password.**
The key check happens later; a rejection at association is the AP declining.
Look for a shunned or pending device in its admin interface — the station
interface presents its own MAC (the radio's address with the locally-administered
bit set, `48:… → 4a:…`), which your router has never seen before. That MAC is
deterministic, so approving it once holds.

## Wi-Fi sleep

Separately from all of the above, the firmware powers the radio down 60 s after
the **last app client** disconnects — see [ASTRO.md](ASTRO.md). Keep Awake on the
dashboard holds a registered client open so that timer never starts. With
auto-join enabled you will usually want Keep Awake too, or the device drops off
your home network 60 s after you close the Benro app.
