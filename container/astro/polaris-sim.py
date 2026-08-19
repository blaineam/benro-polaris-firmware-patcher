#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
polaris-sim — a stand-in for the Benro Polaris' control port.

Speaks the mount's own protocol ("1&<cmd>&3&k:v;...#" in, "<cmd>@k:v;...#" out)
so the real `polaris-mount` binary and the real alignment loop can be exercised
end to end with no hardware attached.

What it models, because these are the things that can break the loop:

  * a mount that is MISALIGNED at startup: its idea of azimuth is offset from
    true north by --az-error degrees, exactly the situation a compass-free cold
    start leaves you in. Everything it reports is in its own wrong frame.
  * finite slew speed, so "goto" takes time and reports twice (start, finish).
  * sidereal tracking that actually holds a sky target when enabled.
  * 527 (set compass), which is how an alignment correction gets applied.
  * pointing jitter and a small non-repeatable settle error (--jitter), so a
    loop that only works with perfect numbers is caught here.

Run:  polaris-sim.py [--port 9090] [--az-error 37.5] [--lat .. --lon ..]
Query its TRUE pointing (what the sky would show) out of band:  --status-port
"""
import argparse, math, socket, socketserver, threading, time, json, datetime, random

DEG = math.pi / 180.0


def jd_now(t=None):
    return (time.time() if t is None else t) / 86400.0 + 2440587.5


def gmst_deg(jd):
    T = (jd - 2451545.0) / 36525.0
    g = (280.46061837 + 360.98564736629 * (jd - 2451545.0)
         + 0.000387933 * T * T - T * T * T / 38710000.0)
    return g % 360.0


def lst_deg(jd, lon):
    return (gmst_deg(jd) + lon) % 360.0


def radec2altaz(ra, dec, lat, lon, jd):
    ha = (lst_deg(jd, lon) - ra) * DEG
    d, p = dec * DEG, lat * DEG
    sinalt = max(-1.0, min(1.0, math.sin(d) * math.sin(p) + math.cos(d) * math.cos(p) * math.cos(ha)))
    alt = math.asin(sinalt)
    az = math.atan2(-math.sin(ha) * math.cos(d),
                    math.cos(p) * math.sin(d) - math.sin(p) * math.cos(d) * math.cos(ha))
    return alt / DEG, (az / DEG) % 360.0


def altaz2radec(alt, az, lat, lon, jd):
    a, A, p = alt * DEG, az * DEG, lat * DEG
    sind = max(-1.0, min(1.0, math.sin(a) * math.sin(p) + math.cos(a) * math.cos(p) * math.cos(A)))
    dec = math.asin(sind)
    ha = math.atan2(-math.sin(A) * math.cos(a),
                    math.cos(p) * math.sin(a) - math.sin(p) * math.cos(a) * math.cos(A))
    return (lst_deg(jd, lon) - ha / DEG) % 360.0, dec / DEG


class Mount:
    """State is kept in TRUE sky terms; what we report is deliberately wrong by
    `az_error` until an alignment corrects it."""

    def __init__(self, args):
        self.lock = threading.RLock()
        self.lat, self.lon = args.lat, args.lon
        self.az_error = args.az_error          # truth = reported + az_error
        self.alt_error = args.alt_error
        self.slew_rate = args.slew_rate        # deg/s
        self.jitter = args.jitter              # deg, non-repeatable settle error
        self.true_alt, self.true_az = 45.0, (0.0 + self.az_error) % 360.0
        self.tracking = False
        self.track_target = None               # (ra, dec) held while tracking
        self.slewing = False
        self.mode = 8                          # Astro
        self.aligned = False
        self.t0 = time.time()
        self.moves = 0

    # ---- what the mount BELIEVES, i.e. what it puts on the wire -----------
    def reported(self):
        with self.lock:
            return ((self.true_az - self.az_error) % 360.0,
                    self.true_alt - self.alt_error)

    def tick(self):
        """Sidereal tracking: hold the sky target, which means alt/az move."""
        with self.lock:
            if self.tracking and self.track_target:
                ra, dec = self.track_target
                self.true_alt, self.true_az = radec2altaz(ra, dec, self.lat, self.lon, jd_now())

    def goto(self, want_az_reported, want_alt_reported, track, done):
        """Slew to a commanded position. The command is in the mount's own
        (possibly wrong) frame — that is the whole point."""
        with self.lock:
            self.slewing = True
            target_az = (want_az_reported + self.az_error) % 360.0
            target_alt = want_alt_reported + self.alt_error
            start_az, start_alt = self.true_az, self.true_alt
        dz = ((target_az - start_az + 540.0) % 360.0) - 180.0
        dalt = target_alt - start_alt
        dist = max(abs(dz), abs(dalt))
        dur = dist / max(self.slew_rate, 0.01)
        steps = max(1, int(dur * 20))
        for i in range(steps):
            time.sleep(min(0.05, dur / steps if steps else 0.05))
            f = (i + 1) / steps
            with self.lock:
                self.true_az = (start_az + dz * f) % 360.0
                self.true_alt = start_alt + dalt * f
        with self.lock:
            # a real mount does not land perfectly
            self.true_az = (self.true_az + random.uniform(-self.jitter, self.jitter)) % 360.0
            self.true_alt += random.uniform(-self.jitter, self.jitter)
            self.slewing = False
            self.moves += 1
            self.tracking = bool(track)
            if self.tracking:
                self.track_target = altaz2radec(self.true_alt, self.true_az,
                                                self.lat, self.lon, jd_now())
        done()

    def set_compass(self, compass_value):
        """527. The app sends (azimuth - 180) % 360, so undo that, then make the
        mount's frame agree with the azimuth it was just told it is looking at."""
        with self.lock:
            claimed_az = (compass_value + 180.0) % 360.0
            self.az_error = ((self.true_az - claimed_az + 540.0) % 360.0) - 180.0

    def status(self):
        with self.lock:
            ra, dec = altaz2radec(self.true_alt, self.true_az, self.lat, self.lon, jd_now())
            raz, ralt = self.reported()
            return {"true_alt_deg": self.true_alt, "true_az_deg": self.true_az,
                    "true_ra_deg": ra, "true_dec_deg": dec,
                    "reported_alt_deg": ralt, "reported_az_deg": raz,
                    "az_error_deg": self.az_error, "tracking": self.tracking,
                    "slewing": self.slewing, "moves": self.moves}


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(0.05)
        mount = self.server.mount
        buf = ""
        last_pose = 0.0
        while not self.server.stopping:
            mount.tick()
            now = time.time()
            if now - last_pose > 0.1:                       # ~10 Hz telemetry
                last_pose = now
                az, alt = mount.reported()
                # 518 carries a quaternion too; the fields our client reads are
                # compass and alt, so those are the ones that must be right.
                self.send(f"518@w:1.0;x:0.0;y:0.0;z:0.0;compass:{az:.6f};alt:{-alt:.6f};")
                self.send(f"517@yaw:{az*DEG:.6f};pitch:{-alt*DEG:.6f};roll:0.000000;")
                self.send(f"284@mode:{mount.mode};track:{1 if mount.tracking else 0};")
            try:
                data = sock.recv(4096)
                if not data:
                    return
                buf += data.decode(errors="replace")
            except socket.timeout:
                continue
            except OSError:
                return
            while "#" in buf:
                msg, buf = buf.split("#", 1)
                self.dispatch(msg + "#")

    def send(self, s):
        try:
            self.request.sendall((s + "#").encode())
        except OSError:
            pass

    def dispatch(self, msg):
        mount = self.server.mount
        parts = msg.strip("#").split("&")
        if len(parts) < 4:
            return
        cmd, argstr = parts[1], parts[3]
        args = {}
        for kv in argstr.split(";"):
            if ":" in kv:
                k, v = kv.split(":", 1)
                args[k] = v
        if self.server.log:
            print(f"[sim] <- {cmd} {args}", flush=True)

        if cmd == "519":
            if args.get("state") == "0":
                with mount.lock:
                    mount.slewing = False
                self.send("519@ret:-1;track:0;")
                return
            # the mount's wire azimuth is signed, measured westward, in
            # (-180, 180) -- hardware rejects anything outside that with ret:-1
            wire = float(args.get("yaw", 0))
            if wire > 180.0 or wire < -180.0:
                print(f"[sim] REFUSING goto: yaw {wire} is outside (-180,180)", flush=True)
                self.send("519@ret:-1;track:0;")
                return
            az = (360.0 - wire) % 360.0 if wire > 0 else (-wire) % 360.0
            alt = float(args.get("pitch", 0))
            track = args.get("track", "0") == "1"
            self.send("519@ret:0;track:0;")                 # slew started
            threading.Thread(
                target=mount.goto, args=(az, alt, track,
                                         lambda: self.send("519@ret:0;track:%d;" % (1 if track else 0))),
                daemon=True).start()
        elif cmd == "531":
            on = args.get("state") == "1"
            with mount.lock:
                mount.tracking = on
                if on:
                    mount.track_target = altaz2radec(mount.true_alt, mount.true_az,
                                                     mount.lat, mount.lon, jd_now())
            self.send(f"531@ret:{1 if on else 0};")
        elif cmd == "527":
            mount.set_compass(float(args.get("compass", 0)))
            self.send("527@ret:0;")
        elif cmd == "530":
            # Star alignment. The caller declares "I am currently pointing at
            # this sky position"; the mount makes its frame agree. Hardware
            # sends the real values in BOTH step:1 and step:2 (captured from
            # the phone app), so act on step:2 and accept step:1 quietly.
            if args.get("step") == "2":
                wire = float(args.get("yaw", 0))
                claimed_az = (360.0 - wire) % 360.0 if wire > 0 else (-wire) % 360.0
                claimed_alt = float(args.get("pitch", 0))
                with mount.lock:
                    mount.az_error = ((mount.true_az - claimed_az + 540.0) % 360.0) - 180.0
                    mount.alt_error = mount.true_alt - claimed_alt
                    mount.aligned = True
                if self.server.log:
                    print(f"[sim] 530 aligned: claimed {claimed_az:.4f}/{claimed_alt:.4f} "
                          f"true {mount.true_az:.4f}/{mount.true_alt:.4f} "
                          f"-> az_error {mount.az_error:.4f}", flush=True)
            self.send("530@ret:0;")
        elif cmd == "284":
            self.send(f"284@mode:{mount.mode};track:{1 if mount.tracking else 0};")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class StatusHandler(socketserver.BaseRequestHandler):
    """A tiny out-of-band channel so a test can ask where the mount REALLY
    points — the equivalent of looking at the sky.

    Speaks minimal HTTP rather than dumping raw JSON on connect, so ordinary
    clients can read it: curl, and polaris-mount's `fetch` (which is how the
    on-device daemon renders the sim's view of the sky). Raw-JSON-on-connect
    looked fine against a hand-rolled reader and failed against both.
    """
    def handle(self):
        try:
            self.request.settimeout(2.0)
            try:
                self.request.recv(4096)          # discard the request line
            except Exception:
                pass
            body = json.dumps(self.server.mount.status()) + "\n"
            self.request.sendall(
                ("HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %d\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n" % len(body)).encode() + body.encode())
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9090)
    ap.add_argument("--status-port", type=int, default=9091)
    # Loopback by default. Bind wider only to drive the sim from another
    # machine (e.g. the real daemon running on the Polaris).
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--lat", type=float, default=40.0)
    ap.add_argument("--lon", type=float, default=-111.9)
    ap.add_argument("--az-error", type=float, default=37.5,
                    help="how wrong the mount's heading is at startup (deg)")
    ap.add_argument("--alt-error", type=float, default=0.0)
    ap.add_argument("--slew-rate", type=float, default=20.0, help="deg/s")
    ap.add_argument("--jitter", type=float, default=0.02,
                    help="non-repeatable settle error per move (deg)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--log", action="store_true")
    a = ap.parse_args()
    if a.seed is not None:
        random.seed(a.seed)

    mount = Mount(a)

    # A real mount tracks on its own. Ticking only inside the client handler
    # made tracking stop the moment a client disconnected -- an artefact that
    # hid whether tracking worked at all.
    def ticker():
        while True:
            mount.tick()
            time.sleep(0.05)
    threading.Thread(target=ticker, daemon=True).start()

    srv = Server((a.bind, a.port), Handler)
    srv.mount = mount; srv.log = a.log; srv.stopping = False
    st = Server((a.bind, a.status_port), StatusHandler)
    st.mount = mount; st.log = False; st.stopping = False
    threading.Thread(target=st.serve_forever, daemon=True).start()
    print(f"[sim] Polaris on {a.bind}:{a.port} (status {a.status_port}); "
          f"az_error={a.az_error} lat={a.lat} lon={a.lon}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
