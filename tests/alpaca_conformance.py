#!/usr/bin/env python3
"""
ASCOM Alpaca conformance checks for polaris-httpd.

Checks the parts of the Alpaca API spec that clients (NINA, Stellarium,
SkySafari, Conform) actually rely on. Reports PASS/FAIL per rule so gaps are
visible rather than assumed away.

    python3 tests/alpaca_conformance.py http://192.168.0.1:8090
"""
import json, sys, urllib.request, urllib.parse, urllib.error

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://192.168.0.1:8090"
DEV  = BASE + "/api/v1/telescope/0"
results = []

def rec(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("  PASS  " if ok else "  FAIL  ") + name + (("   -- " + detail) if detail else ""))

def req(url, method="GET", data=None, timeout=15):
    body = urllib.parse.urlencode(data).encode() if data else None
    r = urllib.request.Request(url, data=body, method=method)
    if body:
        r.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(r, timeout=timeout) as f:
            return f.status, f.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")

def jget(url, **kw):
    code, txt = req(url, **kw)
    try:
        return code, json.loads(txt)
    except Exception:
        return code, None

print("=== Alpaca conformance:", BASE, "===\n")

# ---- management ---------------------------------------------------------
print("[management]")
c, j = jget(BASE + "/management/apiversions?ClientTransactionID=11")
rec("apiversions returns 200 + JSON", c == 200 and j is not None, f"http {c}")
if j:
    rec("apiversions Value is an array of ints",
        isinstance(j.get("Value"), list) and all(isinstance(v, int) for v in j["Value"]),
        str(j.get("Value")))
    rec("apiversions echoes ClientTransactionID", j.get("ClientTransactionID") == 11,
        str(j.get("ClientTransactionID")))

c, j = jget(BASE + "/management/v1/configureddevices")
ok = bool(j) and isinstance(j.get("Value"), list) and j["Value"]
rec("configureddevices returns a device list", ok)
if ok:
    d = j["Value"][0]
    for k in ("DeviceName", "DeviceType", "DeviceNumber", "UniqueID"):
        rec(f"configureddevices has {k}", k in d, str(d.get(k)))
    rec("DeviceType == Telescope", d.get("DeviceType") == "Telescope", str(d.get("DeviceType")))
    rec("DeviceNumber is an int", isinstance(d.get("DeviceNumber"), int))

c, j = jget(BASE + "/management/v1/description")
rec("description has ServerName", bool(j) and "ServerName" in (j.get("Value") or {}))

# ---- response envelope --------------------------------------------------
print("\n[response envelope]")
c, j = jget(DEV + "/connected?ClientTransactionID=42")
if j:
    for k in ("Value", "ClientTransactionID", "ServerTransactionID", "ErrorNumber", "ErrorMessage"):
        rec(f"envelope has {k}", k in j)
    rec("ClientTransactionID echoed exactly", j.get("ClientTransactionID") == 42,
        str(j.get("ClientTransactionID")))
    rec("ErrorNumber is 0 on success", j.get("ErrorNumber") == 0, str(j.get("ErrorNumber")))
    rec("ErrorMessage empty on success", j.get("ErrorMessage") == "", repr(j.get("ErrorMessage")))

# ServerTransactionID must be unique and increasing
ids = []
for _ in range(3):
    _, jj = jget(DEV + "/connected")
    if jj: ids.append(jj.get("ServerTransactionID"))
rec("ServerTransactionID strictly increases",
    len(ids) == 3 and all(isinstance(i, int) for i in ids) and ids[0] < ids[1] < ids[2], str(ids))

# missing ClientTransactionID -> spec says treat as absent, do not error
c, j = jget(DEV + "/connected")
rec("missing ClientTransactionID is not an error", bool(j) and j.get("ErrorNumber") == 0)

# ---- case insensitivity (spec: parameter names are case-insensitive) ----
print("\n[case handling]")
c, j = jget(DEV + "/connected?clienttransactionid=7")
rec("ClientTransactionID is case-insensitive", bool(j) and j.get("ClientTransactionID") == 7,
    str(j.get("ClientTransactionID") if j else None))

# ---- required telescope members ----------------------------------------
print("\n[required telescope members]")
required = ["connected","description","driverinfo","driverversion","interfaceversion","name",
            "supportedactions","alignmentmode","altitude","azimuth","aperturearea","aperturediameter",
            "athome","atpark","canfindhome","canpark","canpulseguide","cansetdeclinationrate",
            "cansetguiderates","cansetpark","cansetpierside","cansetrightascensionrate",
            "cansettracking","canslew","canslewaltaz","canslewaltazasync","canslewasync",
            "cansync","cansyncaltaz","canunpark","declination","declinationrate","doesrefraction",
            "equatorialsystem","focallength","guideratedeclination","guideraterightascension",
            "ispulseguiding","rightascension","rightascensionrate","sideofpier","siderealtime",
            "siteelevation","sitelatitude","sitelongitude","slewing","slewsettletime",
            "targetdeclination","targetrightascension","tracking","trackingrate","trackingrates",
            "utcdate","axisrates","canmoveaxis","destinationsideofpier"]
missing, errored = [], []
for m in required:
    c, j = jget(f"{DEV}/{m}")
    if j is None:
        missing.append(m)
    # 1024 NotImplemented and 1026 ValueNotSet are both legitimate per spec --
    # ValueNotSet is exactly right for target coordinates before one is set.
    elif j.get("ErrorNumber") not in (0, 1024, 1026):
        errored.append((m, j.get("ErrorNumber")))
rec(f"all {len(required)} required members respond with valid JSON",
    not missing, "no JSON from: " + ", ".join(missing[:10]) if missing else "")
rec("members return 0 / 1024 NotImplemented / 1026 ValueNotSet", not errored, str(errored[:5]))

# ---- error semantics ----------------------------------------------------
print("\n[error semantics]")
c, j = jget(DEV + "/notarealmethod")
rec("unknown method still returns HTTP 200 + envelope", c == 200 and j is not None, f"http {c}")
if j:
    rec("unknown method sets ErrorNumber 1024 (NotImplemented)",
        j.get("ErrorNumber") == 1024, str(j.get("ErrorNumber")))
    rec("unknown method has non-empty ErrorMessage", bool(j.get("ErrorMessage")))

c, j = jget(BASE + "/api/v1/telescope/9/connected")
rec("bad device number is rejected", (j is not None and j.get("ErrorNumber") != 0) or c >= 400,
    f"http {c} err {j.get('ErrorNumber') if j else None}")

c, j = jget(BASE + "/api/v1/camera/0/connected")
rec("unsupported device type is rejected", (j is not None and j.get("ErrorNumber") != 0) or c >= 400,
    f"http {c}")

# ---- value sanity -------------------------------------------------------
print("\n[value sanity]")
c, j = jget(DEV + "/rightascension")
ra = j.get("Value") if j else None
rec("RightAscension in hours 0..24", isinstance(ra, (int, float)) and 0 <= ra < 24, str(ra))
c, j = jget(DEV + "/declination")
dec = j.get("Value") if j else None
rec("Declination in degrees -90..90", isinstance(dec, (int, float)) and -90 <= dec <= 90, str(dec))
c, j = jget(DEV + "/siderealtime")
st = j.get("Value") if j else None
rec("SiderealTime in hours 0..24",
    isinstance(st, (int, float)) and 0 <= st < 24, str(st))

# ---- PUT handling -------------------------------------------------------
print("\n[PUT handling]")
c, j = jget(DEV + "/connected", method="PUT", data={"Connected": "true", "ClientTransactionID": "5"})
rec("PUT connected accepted", c == 200 and j is not None and j.get("ErrorNumber") == 0, f"http {c}")
rec("PUT echoes ClientTransactionID from BODY", bool(j) and j.get("ClientTransactionID") == 5,
    str(j.get("ClientTransactionID") if j else None))

c, j = jget(DEV + "/slewtocoordinatesasync", method="PUT", data={"RightAscension": "abc"})
rec("PUT with bad/missing params returns an Alpaca error",
    bool(j) and j.get("ErrorNumber") != 0, str(j.get("ErrorNumber") if j else None))

# ---- summary ------------------------------------------------------------
p = sum(1 for _, ok, _ in results if ok)
print(f"\n=== {p}/{len(results)} passed, {len(results)-p} failed ===")
for n, ok, d in results:
    if not ok:
        print("  FAILED:", n, ("-- " + d) if d else "")
sys.exit(0 if p == len(results) else 1)
