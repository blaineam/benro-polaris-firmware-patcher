#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
sim-verify — prove the alignment loop works, repeatably, with no hardware.

Starts polaris-sim (a mount deliberately misaligned at startup) and drives it
with the REAL polaris-mount binary over the REAL protocol, checking against the
simulator's ground truth:

  1. alignment     one simulated plate solve + one 527 removes the heading error
  2. pointing      after aligning, gotos land on target
  3. repeatability N align+goto cycles in a row all land, with no drift
  4. tracking      with tracking on, a target holds as the sky rotates
  5. safety        motion outside the allowed envelope is refused

Two ways to get the "plate solve":

  default    the simulator's TRUE pointing converted to J2000 RA/Dec plus noise
             -- fast, and enough to test the loop's control logic.

  --camera   RENDER what the camera would actually see with polaris-skysim,
             then run the real polaris-extract and polaris-solve over it. No
             ground truth is used anywhere in the measurement path: the mount
             moves, the sky is drawn from the catalogue at wherever it ended up,
             and the solver has to work it out. This is the honest end-to-end
             test, and it is what a motors-in-the-loop night looks like without
             a camera or a sky.
"""
import argparse, json, math, os, random, shutil, socket, subprocess, sys, tempfile, time


def status(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        return json.loads(s.recv(65536).decode().strip())


class Mount:
    def __init__(self, binary, port, lat, lon):
        self.base = [binary, "--host", "127.0.0.1", "--port", str(port),
                     "--lat", str(lat), "--lon", str(lon)]

    def __call__(self, *args, timeout=180, allow_fail=False):
        cmd = self.base + [str(a) for a in args]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = p.stdout.strip().splitlines()
        if not out:
            if allow_fail:
                return {"_refused": True, "_stderr": p.stderr.strip(), "_rc": p.returncode}
            raise RuntimeError(f"no output from {' '.join(cmd)}: {p.stderr[-400:]}")
        return json.loads(out[-1])


def sep_altaz(alt1, az1, alt2, az2):
    """angular separation of two pointings, in degrees"""
    a1, z1, a2, z2 = map(math.radians, (alt1, az1, alt2, az2))
    v = math.sin(a1) * math.sin(a2) + math.cos(a1) * math.cos(a2) * math.cos(z1 - z2)
    return math.degrees(math.acos(max(-1.0, min(1.0, v))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mount", default="./polaris-mount")
    ap.add_argument("--sim", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "polaris-sim.py"))
    ap.add_argument("--port", type=int, default=9390)
    ap.add_argument("--status-port", type=int, default=9391)
    ap.add_argument("--lat", type=float, default=40.0)
    ap.add_argument("--lon", type=float, default=-111.9)
    ap.add_argument("--az-error", type=float, default=37.5)
    ap.add_argument("--cycles", type=int, default=8)
    ap.add_argument("--solve-noise-arcsec", type=float, default=30.0)
    ap.add_argument("--tolerance-arcmin", type=float, default=12.0)
    ap.add_argument("--track-seconds", type=float, default=20.0)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--camera", action="store_true",
                    help="render + extract + solve real frames instead of using truth")
    ap.add_argument("--skysim", default="./polaris-skysim")
    ap.add_argument("--extract", default="./polaris-extract")
    ap.add_argument("--solve", default="./polaris-solve")
    ap.add_argument("--index", action="append", default=[])
    ap.add_argument("--focal-mm", type=float, default=400.0)
    ap.add_argument("--cam-width", type=int, default=8192)
    ap.add_argument("--cam-height", type=int, default=6144)
    a = ap.parse_args()

    random.seed(a.seed)
    noise = a.solve_noise_arcsec / 3600.0
    tmpdir = tempfile.mkdtemp(prefix="polaris-simcam-")
    if a.camera and not a.index:
        print("--camera needs at least one --index"); return 2
    sim = subprocess.Popen(
        [sys.executable, a.sim, "--port", str(a.port), "--status-port", str(a.status_port),
         "--lat", str(a.lat), "--lon", str(a.lon), "--az-error", str(a.az_error),
         "--seed", str(a.seed)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    failures = []
    m = Mount(a.mount, a.port, a.lat, a.lon)

    def simulated_solve():
        """what a plate solve of the current frame would report (J2000)"""
        st = status(a.status_port)
        rd = m("altaz2radec", "--alt", f"{st['true_alt_deg']:.6f}", "--az", f"{st['true_az_deg']:.6f}")
        if not a.camera:
            return (rd["ra_deg"] + random.gauss(0, noise) / max(0.05, math.cos(math.radians(rd["dec_deg"]))),
                    rd["dec_deg"] + random.gauss(0, noise), st)

        # Render what the camera would see, then actually solve it. The true
        # pointing is used ONLY to draw the sky -- the solver is told nothing
        # but the frame and a hint from the mount's own (wrong) idea of pose.
        idx = []
        for f in a.index:
            idx += ["--index", f]
        frame = os.path.join(tmpdir, "frame.jpg")
        stars = os.path.join(tmpdir, "stars.txt")
        subprocess.run([a.skysim] + idx +
                       ["--ra", f"{rd['ra_deg']:.6f}", "--dec", f"{rd['dec_deg']:.6f}",
                        "--roll", f"{random.uniform(0, 360):.2f}",
                        "--focal-mm", str(a.focal_mm),
                        "--width", str(a.cam_width), "--height", str(a.cam_height),
                        "--out", frame], capture_output=True, text=True, timeout=300)
        with open(stars, "w") as fh:
            subprocess.run([a.extract, "--jpeg", frame, "--downsample", "4",
                            "--max-stars", "200"], stdout=fh, stderr=subprocess.DEVNULL,
                           timeout=300)
        # the hint is what the MOUNT believes, not the truth
        hint = m("altaz2radec", "--alt", f"{st['reported_alt_deg']:.6f}",
                 "--az", f"{st['reported_az_deg']:.6f}")
        p = subprocess.run([a.solve] + idx +
                           ["--stars", stars, "--width", str(a.cam_width),
                            "--height", str(a.cam_height), "--focal-mm", str(a.focal_mm),
                            "--sensor-mm", "36",
                            "--ra", f"{hint['ra_deg']:.6f}", "--dec", f"{hint['dec_deg']:.6f}",
                            "--radius", "45", "--cpulimit", "60"],
                           capture_output=True, text=True, timeout=300)
        try:
            out = json.loads(p.stdout.strip().splitlines()[-1])
        except Exception:
            raise RuntimeError(f"solver produced nothing: {p.stdout[-200:]} {p.stderr[-200:]}")
        if not out.get("solved"):
            raise RuntimeError(f"real solve FAILED on a rendered frame: {out}")
        return out["ra_deg"], out["dec_deg"], st

    try:
        for _ in range(50):
            try:
                status(a.status_port); break
            except OSError:
                time.sleep(0.2)
        else:
            print("FAIL: simulator never started"); return 1

        # ---------------------------------------------------------- 1. align
        st = status(a.status_port)
        print(f"=== cold start: the mount's heading is wrong by {a.az_error} deg ===")
        print(f"    it believes az {st['reported_az_deg']:.3f}; the sky says {st['true_az_deg']:.3f}")
        ra, dec, _ = simulated_solve()
        r = m("align", "--solved-ra", f"{ra:.6f}", "--solved-dec", f"{dec:.6f}")
        st = status(a.status_port)
        ok = abs(st["az_error_deg"]) <= 0.05
        print(f"[1] align: correction {r['az_error_deg']:+.3f} deg -> residual "
              f"{st['az_error_deg']*3600:+.1f} arcsec  {'OK' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"alignment left {st['az_error_deg']:.3f} deg of heading error")

        # --------------------------------------------- 2/3. point + repeat
        print(f"[2] {a.cycles} align+goto cycles (tolerance {a.tolerance_arcmin:.0f} arcmin)")
        targets = [(83.822, -5.391), (279.234, 38.784), (101.287, -16.716),
                   (213.915, 19.182), (310.358, 45.280), (165.932, 61.751),
                   (88.793, 7.407), (252.166, -26.432)]
        worst = 0.0
        for i in range(a.cycles):
            tra, tdec = targets[i % len(targets)]
            want = m("radec2altaz", "--ra", tra, "--dec", tdec)
            if not (5.0 < want["alt_deg"] < 85.0):
                print(f"    cycle {i}: target below the allowed band (alt {want['alt_deg']:.1f}) — skipped")
                continue
            t_cmd = time.time()
            g = m("goto-radec", "--ra", tra, "--dec", tdec)   # tracking on at arrival
            if not g.get("goto"):
                failures.append(f"cycle {i}: goto refused: {g}")
                continue
            st = status(a.status_port)
            # where SHOULD it be pointing, right now, for that target?
            want_now = m("radec2altaz", "--ra", tra, "--dec", tdec)
            err = sep_altaz(st["true_alt_deg"], st["true_az_deg"],
                            want_now["alt_deg"], want_now["az_deg"])
            worst = max(worst, err)
            mark = "OK" if err * 60 <= a.tolerance_arcmin else "FAIL"
            if err * 60 > a.tolerance_arcmin:
                failures.append(f"cycle {i}: landed {err*60:.1f} arcmin from target")
            d_alt = (st["true_alt_deg"] - want_now["alt_deg"]) * 60
            d_az = ((st["true_az_deg"] - want_now["az_deg"] + 540) % 360 - 180) * 60 \
                   * math.cos(math.radians(want_now["alt_deg"]))
            print(f"    cycle {i}: {tra:7.3f},{tdec:+7.3f} (alt {want_now['alt_deg']:5.1f}) -> "
                  f"off {err*60:6.2f}' [dalt {d_alt:+6.2f}' daz {d_az:+6.2f}'] {mark} "
                  f"slew {time.time()-t_cmd:.1f}s")
            ra, dec, st_now = simulated_solve()      # re-solve and re-align, as a session would
            al = m("align", "--solved-ra", f"{ra:.6f}", "--solved-dec", f"{dec:.6f}", allow_fail=True)
            if al.get("aligned") is False:
                print(f"      (align refused from alt {al.get('alt_deg', float('nan')):.1f} deg — "
                      f"{al.get('error_amplification', 0):.0f}x amplification — correctly skipped)")
        print(f"    worst over {a.cycles} cycles: {worst*60:.2f} arcmin")

        # -------------------------------------------------------- 4. tracking
        print(f"[3] tracking for {a.track_seconds:.0f} s")
        up = [(r, d) for (r, d) in targets
              if 20.0 < m("radec2altaz", "--ra", r, "--dec", d)["alt_deg"] < 80.0]
        if up:
            tra, tdec = up[0]
            want = m("radec2altaz", "--ra", tra, "--dec", tdec)
            m("goto-radec", "--ra", tra, "--dec", tdec)
            m("track", "on")
            st0 = status(a.status_port)
            time.sleep(a.track_seconds)
            st1 = status(a.status_port)
            moved = sep_altaz(st0["true_alt_deg"], st0["true_az_deg"],
                              st1["true_alt_deg"], st1["true_az_deg"])
            now = m("radec2altaz", "--ra", tra, "--dec", tdec)
            held = sep_altaz(st1["true_alt_deg"], st1["true_az_deg"],
                             now["alt_deg"], now["az_deg"])
            print(f"    mount moved {moved*60:.2f} arcmin in alt/az; target held to "
                  f"{held*3600:.1f} arcsec")
            if held * 3600 > 120:
                failures.append(f"tracking lost the target by {held*3600:.0f} arcsec")
            if moved * 3600 < 10:
                failures.append("mount never moved while tracking")
        else:
            print("    no target in the allowed band right now; skipped")

        # --------------------------------------------------------- 5. safety
        print("[4] safety rails")
        # the zenith is at RA = local sidereal time, Dec = latitude
        lst = m("radec2altaz", "--ra", 0, "--dec", 0)["lst_deg"]
        zen = m("align", "--solved-ra", f"{lst:.6f}", "--solved-dec", str(a.lat), allow_fail=True)
        zenith_refused = zen.get("aligned") is False or zen.get("_refused")
        print(f"    aligning from near the zenith: "
              f"{'refused OK' if zenith_refused else 'ACCEPTED — FAIL'}")
        if not zenith_refused:
            failures.append("safety: an alignment from near the zenith was accepted")
        checks = [("below the horizon", ("goto", "--alt", "-10", "--az", "180")),
                  ("above the alt limit", ("goto", "--alt", "89", "--az", "180")),
                  ("a slew bigger than --max-slew", ("goto", "--alt", "45", "--az", "180", "--max-slew", "1"))]
        for label, args in checks:
            r = m(*args, allow_fail=True)
            refused = r.get("_refused") or r.get("goto") is False
            print(f"    {label}: {'refused OK' if refused else 'ACCEPTED — FAIL'}")
            if not refused:
                failures.append(f"safety: {label} was accepted")
    except Exception as e:
        failures.append(f"exception: {e}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
        sim.terminate()
        try: sim.wait(timeout=5)
        except Exception: sim.kill()

    print()
    if failures:
        print(f"RESULT: {len(failures)} FAILURE(S)")
        for f in failures:
            print("  -", f)
        return 1
    print("RESULT: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
