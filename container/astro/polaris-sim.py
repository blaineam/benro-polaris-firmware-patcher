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
  * imperfect tracking (--track-drift, arcsec/min), so a guider has something
    real to correct. Drift restarts after each correction, because a mount
    drifts away from wherever you just put it.

KNOWN LIMITATION -- coordinates are ~0.2 deg off polaris-mount's:

    same alt/az, same instant:
        sim   -> RA 304.514182  Dec 54.543075
        mount -> RA 304.321539  Dec 54.457980

polaris-mount applies precession, nutation and aberration (validated to 8.2" vs
astropy); this sim uses a plain conversion without them. So a test that compares
a sim-derived RA/Dec against a mount-derived one will see a constant ~0.2 deg
offset that is NOT a product bug. Compare sim-to-sim or mount-to-mount, or
expect that offset.

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
        # Jog velocity per axis, deg/s, set by 513/514/521 (fast, continuous)
        # and 532/533/534 (slow, latched). `jog_seen` is the last time a fast
        # command arrived: fast jog is a dead-man stream on real hardware, and
        # modelling it as one is the whole point -- a client that stops sending
        # must visibly stop the mount here too.
        self.jog = {"pan": 0.0, "tilt": 0.0, "rot": 0.0}
        self.jog_seen = {"pan": 0.0, "tilt": 0.0, "rot": 0.0}
        self.jog_latched = {"pan": False, "tilt": False, "rot": False}
        self.jog_t = time.time()
        # A running capture programme (272 timelapse / 271 panorama): the sim
        # counts frames down on the SAME clock the app polls, so the whole
        # progress loop is exercisable with no hardware. `prog_total<0` = a
        # timelapse's unlimited default, which never reaches zero.
        self.prog = None          # None | dict(cmd, remaining, total, per_s, t0)
        self.push_q = []          # unsolicited frames for the handler to flush
        self.aligned = False
        self.track_drift = getattr(args, "track_drift", 0.0)   # arcsec/min
        self.track_t0 = None
        self.drift_accum = 0.0
        self.tracking = False
        self.track_target = None               # (ra, dec) held while tracking
        self.slewing = False
        self.mode = 8                          # Astro
        self.grail = False                     # Holy Grail ramp configured on
        self.aligned = False
        self.t0 = time.time()
        self.moves = 0

    # ---- what the mount BELIEVES, i.e. what it puts on the wire -----------
    def reported(self):
        with self.lock:
            return ((self.true_az - self.az_error) % 360.0,
                    self.true_alt - self.alt_error)

    def tick(self):
        """Sidereal tracking: hold the sky target, which means alt/az move.

        With --track-drift the hold is imperfect, accumulating error the way a
        real mount does. That is what a guider is for."""
        with self.lock:
            if self.tracking and self.track_target:
                ra, dec = self.track_target
                self.true_alt, self.true_az = radec2altaz(ra, dec, self.lat, self.lon, jd_now())
                if self.track_drift:
                    if self.track_t0 is None:
                        self.track_t0 = time.time()
                    mins = (time.time() - self.track_t0) / 60.0
                    self.drift_accum = self.track_drift * mins / 3600.0   # deg
                    self.true_az = (self.true_az + self.drift_accum) % 360.0
            else:
                self.track_t0 = None

            # ---- programme countdown ---------------------------------------
            if self.prog is not None:
                el = time.time() - self.prog["t0"]
                shot = int(el / self.prog["per_s"]) if self.prog["per_s"] > 0 else 0
                if self.prog["total"] >= 0:
                    self.prog["remaining"] = max(0, self.prog["total"] - shot)
                    if self.prog["remaining"] == 0:
                        # Push-driven programmes (HDR step:3, SUN step:4)
                        # announce completion with a pushed frame; polled ones
                        # (timelapse/path-lapse/pano/focus) just let their poll
                        # read 0.
                        if self.prog.get("done_push"):
                            self.push_q.append(self.prog["done_push"])
                        self.prog = None      # finished
                # unlimited: remaining stays a sentinel that never hits 0

            # ---- jog integration -------------------------------------------
            now = time.time()
            dt = now - self.jog_t
            self.jog_t = now
            if dt > 0:
                for axis, v in self.jog.items():
                    if v == 0.0:
                        continue
                    # A fast-jog stream that stopped arriving means the client
                    # went away. Real hardware MAY keep going (nobody has
                    # established that it stops), so the sim models the
                    # PESSIMISTIC case only for the latched family and stops
                    # the continuous one after 250 ms of silence -- which is
                    # what lets a test tell "the server kept renewing" from
                    # "the server stopped".
                    if not self.jog_latched[axis] and now - self.jog_seen[axis] > 0.25:
                        self.jog[axis] = 0.0
                        continue
                    if axis == "pan":
                        self.true_az = (self.true_az + v * dt) % 360.0
                    elif axis == "tilt":
                        self.true_alt = max(-90.0, min(90.0, self.true_alt + v * dt))
                    # rot has no effect on the reported alt/az frame here

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
                # Drift restarts from wherever we were just put. Without this the
                # accumulated error is re-applied the instant a goto finishes, so
                # no correction can ever land -- which looks like a guider bug and
                # is really a modelling one. A real mount drifts away from its
                # current position, it does not teleport back onto an old error.
                self.track_t0 = time.time()
                self.drift_accum = 0.0
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
                    "track_drift_deg": self.drift_accum,
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
            with mount.lock:
                pending = mount.push_q; mount.push_q = []
            for frame in pending:
                self.send(frame)
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
        elif cmd in ("513", "514", "521"):
            # Fast/continuous jog: speed:<signed>, magnitude 100..2500, and it
            # must keep arriving to sustain motion. Calibration from the alpaca
            # driver's measured table: raw 2500 ~= 8.92 deg/s.
            axis = {"513": "pan", "514": "tilt", "521": "rot"}[cmd]
            try:
                raw = float(args.get("speed", 0))
            except ValueError:
                raw = 0.0
            with mount.lock:
                mount.jog[axis] = (raw / 2500.0) * 8.92
                mount.jog_seen[axis] = time.time()
                mount.jog_latched[axis] = False
            self.send(f"{cmd}@ret:0;")
        elif cmd in ("532", "533", "534"):
            # Slow/latched jog: state:1 presses, state:0 releases, and a press
            # with no release runs forever -- modelled faithfully, because that
            # is the hazard the whole dead-man design exists to contain.
            axis = {"532": "pan", "533": "tilt", "534": "rot"}[cmd]
            state = args.get("state", "0")
            key = args.get("key", "0")
            try:
                level = int(args.get("level", 1))
            except ValueError:
                level = 1
            with mount.lock:
                if state == "0":
                    mount.jog[axis] = 0.0
                    mount.jog_latched[axis] = False
                else:
                    sign = -1.0 if key == "1" else 1.0
                    mount.jog[axis] = sign * level * 0.5      # deg/s per gear
                    mount.jog_latched[axis] = True
                    mount.jog_seen[axis] = time.time()
            self.send(f"{cmd}@ret:0;")
        elif cmd == "523":
            # SP_GIMBAL_POS_RESET -- re-zero one axis.
            with mount.lock:
                if args.get("axis") == "1":
                    mount.true_az = mount.az_error % 360.0
                elif args.get("axis") == "2":
                    mount.true_alt = 0.0
            self.send("523@ret:0;")
        elif cmd == "808":
            # Registration. The real head answers it and starts counting the
            # client for the wifi-power timer.
            self.send("808@ret:0;")
        elif cmd == "778":
            self.send("778@capacity:82;charge:0;")
        elif cmd == "775":
            self.send("775@status:1;totalspace:62914560;freespace:41943040;usespace:20971520;")
        elif cmd in ("265", "266", "267", "268", "275"):
            # Camera option lists: RD:0=available, V=selected index, R=csv.
            lists = {
                "268": "1/1000,1/500,1/250,1/125,1/60,1/30,1/15,1/8,1/4,0.5,1",
                "275": "1.4,2,2.8,4,5.6,8,11,16,22",
                "265": "Auto,100,200,400,800,1600,3200,6400",
                "267": "-2,-1.7,-1.3,-1,-0.7,-0.3,0,0.3,0.7,1,1.3,1.7,2",
                "266": "Auto,Daylight,Cloudy,Shade,Tungsten,Fluorescent",
            }
            self.send(f"{cmd}@RD:0;V:4;R:{lists[cmd]};")
        elif cmd == "280":
            step = args.get("step")
            if step == "1":
                # HDR is always 3 frames; the head pushes step:5 remaining then step:3.
                with mount.lock:
                    mount.prog = {"cmd": "280", "per_s": 1.5, "total": 3,
                                  "remaining": 3, "t0": time.time(),
                                  "done_push": "280@step:3;ret:0;"}
                self.send("280@step:5;ret:3;")
            elif step == "4":
                with mount.lock:
                    mount.prog = None
                self.send("280@step:4;#" if False else "280@step:4;")
        elif cmd == "270":
            step = args.get("step")
            if step in ("1", "2"):
                self.send(f"270@step:{step};ret:0;")
            elif step in ("3", "8"):
                try:
                    num = int(args.get("num", "14"))
                except ValueError:
                    num = 14
                with mount.lock:
                    mount.prog = {"cmd": "270", "per_s": 1.0, "total": num,
                                  "remaining": num, "t0": time.time()}
                self.send(f"270@step:{step};ret:0;")
            elif step == "7":
                r = -1
                with mount.lock:
                    if mount.prog and mount.prog["cmd"] == "270":
                        r = mount.prog["remaining"]
                self.send(f"270@step:7;remainNum:{r};")
            elif step in ("6", "10"):
                with mount.lock:
                    mount.prog = None
                self.send(f"270@step:{step};ret:0;")
        elif cmd == "311":
            self.send("311@ret:0;")   # focus adjust ack
        elif cmd == "285":            # set mode
            try: mount.mode = int(args.get("mode", "1"))
            except ValueError: pass
            self.send(f"285@mode:{mount.mode};ret:0;")
        elif cmd == "520":            # AHRS state
            self.send("520@ret:0;")
        elif cmd == "527":            # set yaw / compass alignment
            with mount.lock:
                mount.aligned = True
            self.send("527@ret:0;")
        elif cmd == "536":            # half-speed flag
            self.send("536@ret:0;")
        elif cmd == "517":            # mechanical pose, radians
            with mount.lock:
                az, alt = mount.reported()
            self.send(f"517@yaw:{az*DEG:.6f};pitch:{-alt*DEG:.6f};roll:0.000000;")
        elif cmd == "272":
            # Timelapse AND path-lapse ride this opcode. A plain timelapse sends
            # one point; a path-lapse sends 2..8, each with a gimbal pose and a
            # per-leg count. Either way the AUTHORITATIVE frame total is photoCnt
            # in SEND_END (step 3) -- summing the legs -- so the countdown keys
            # off that, not off any single point's count.
            step = args.get("step")
            if step == "1":
                with mount.lock:
                    mount.prog = {"cmd": "272", "per_s": 1.0, "total": -1,
                                  "remaining": -1, "t0": time.time()}
            elif step == "2":
                para = args.get("para", "0,0").split(",")
                try:
                    interval = float(para[0])
                except (ValueError, IndexError):
                    interval = 1.0
                with mount.lock:
                    if not (mount.prog and mount.prog["cmd"] == "272"):
                        mount.prog = {"cmd": "272", "total": -1,
                                      "remaining": -1, "t0": time.time()}
                    mount.prog["per_s"] = max(0.2, interval)
            elif step == "3":
                # SEND_END: photoCnt is the definitive total (-1 = unlimited).
                try:
                    total = int(args.get("photoCnt", "-1"))
                except ValueError:
                    total = -1
                with mount.lock:
                    if not (mount.prog and mount.prog["cmd"] == "272"):
                        mount.prog = {"cmd": "272", "per_s": 1.0, "t0": time.time()}
                    mount.prog["total"] = total
                    mount.prog["remaining"] = total if total >= 0 else -1
                    mount.prog["t0"] = time.time()
            elif step == "4":
                # remaining-count poll: reply, and the app re-asks
                r = -1
                with mount.lock:
                    if mount.prog and mount.prog["cmd"] == "272":
                        r = mount.prog["remaining"]
                self.send(f"272@ret:{r};")
            elif step == "7":
                with mount.lock:
                    mount.prog = None
                self.send("272@ret:0;")
        elif cmd == "277":
            # SUN: a scheduled solar lapse. Push-driven like HDR -- the head
            # reserves the window, does the solar GOTO, then shoots and pushes
            # progress, ending with step:4. The sim skips the wait and runs a
            # brisk demo countdown so the whole flow is exercisable.
            step = args.get("step")
            if step == "1":
                try:
                    interval = float(args.get("interval", "10"))
                except ValueError:
                    interval = 10.0
                with mount.lock:
                    mount.mode = 8
                    mount.prog = {"cmd": "277", "per_s": max(0.2, interval / 4.0),
                                  "total": 4, "remaining": 4, "t0": time.time(),
                                  "done_push": "277@step:4;ret:0;"}
                # Acknowledge the reservation and report the first remaining count.
                self.send("277@step:5;ret:4;")
            elif step in ("2", "3"):    # cancel / end
                with mount.lock:
                    mount.prog = None
                self.send(f"277@step:{step};ret:0;")
        elif cmd == "271":
            step = args.get("step")
            if step == "2":
                num = 0
                for kvp in args.get("para", "").split(","):
                    pass
                try:
                    num = int(args.get("num", "0"))
                except ValueError:
                    num = 0
                with mount.lock:
                    # ~2 s per frame is plausible and keeps the demo brisk
                    mount.prog = {"cmd": "271", "per_s": 2.0, "total": num,
                                  "remaining": num, "t0": time.time()}
                self.send("271@ret:0;")
            elif step == "3":
                r = -1
                with mount.lock:
                    if mount.prog and mount.prog["cmd"] == "271":
                        r = mount.prog["remaining"]
                self.send(f"271@ret:{r};")
            elif step in ("6",):
                with mount.lock:
                    mount.prog = None
                self.send("271@ret:0;")
            elif step in ("12", "13"):
                self.send(f"271@ret:0;")
        elif cmd == "305":
            # HOLY GRAIL — the day->night exposure ramp CONFIG (not a run). The
            # head stores what it is sent and, WITH the Optical Matrix Sensor
            # Module accessory, meters and ramps. The sim has no accessory, so it
            # acks every SET_ and answers the brightness poll with a placeholder.
            step = args.get("step")
            if step == "1":                       # SET_GRAIL_MODEL
                with mount.lock:
                    mount.grail = args.get("state", "0") == "1"
                self.send("305@step:1;ret:0;")
            elif step in ("3", "5", "7", "9", "11"):   # SET_PRIORITY/ISO/F/SHUTTER/CURVE
                self.send(f"305@step:{step};ret:0;")
            elif step == "13":                    # GET_BRIGHTNESS_RUNTIME
                # A real value needs the OMS accessory; 0.0 stands in for "no
                # reading" so the readback path is exercisable.
                self.send("305@step:13;brightness:0.0;")
            elif step in ("2", "4", "6", "8", "10", "12"):   # GET_*
                self.send(f"305@step:{step};ret:0;")
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
    # Real mounts do not track perfectly. Without this the sim holds a target
    # forever and a guider has nothing to correct, which would "pass" a guiding
    # test that has never actually guided anything.
    ap.add_argument("--track-drift", type=float, default=0.0,
                    help="tracking error in arcsec/min, applied in azimuth")
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
