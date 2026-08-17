#!/usr/bin/env python3
"""Generate the /app boot hook that authorises SSH public key(s) on the Polaris.

The stock firmware already runs OpenSSH: /etc/init.d/rcS ends with
`/usr/local/bin/sshd`, and the stock sshd_config has `PermitRootLogin yes` with
`AuthorizedKeysFile .ssh/authorized_keys`. What it does NOT have is any key in
/root/.ssh, so the only way in is the root password.

/app/bootapp (in the appfs, run at boot by /etc/init.d/S10mpp) already sources an
OPTIONAL hook script if one exists:

    if [ -f "/app/network_telnetd.sh" ];then cd /app; ./network_telnetd.sh; fi

so authorising a key needs no edit to any existing firmware file — we only add
the hook script the stock bootapp already looks for. This module validates the
key material and emits that script.

    gen_ssh_hook.py --keys <file> --hook-name <name> [--out <file>]

Keys are validated strictly (type allow-list + base64 + SSH wire format), and
their SHA256 fingerprints are printed to stderr so the caller can log them.
Exits non-zero on anything that is not a well-formed public key.
"""
import argparse
import base64
import hashlib
import struct
import sys

# Public-key algorithms OpenSSH 7.8p1 (what the device ships) accepts in
# authorized_keys. Deliberately no ssh-dss: sshd 7.8 rejects it by default.
ALLOWED = (
    "ssh-ed25519",
    "ssh-rsa",
    "rsa-sha2-256",
    "rsa-sha2-512",
    "ecdsa-sha2-nistp256",
    "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
    "sk-ssh-ed25519@openssh.com",
    "sk-ecdsa-sha2-nistp256@openssh.com",
)

HEREDOC_TAG = "POLARIS_SSH_KEYS_EOF"


def fail(msg):
    sys.stderr.write("[ssh-key] ERROR: %s\n" % msg)
    sys.exit(1)


def parse_key(line, lineno):
    """Validate one authorized_keys line; return (line, fingerprint)."""
    if "PRIVATE KEY" in line:
        fail("line %d looks like a PRIVATE key. Pass the .pub file, never the "
             "private one." % lineno)
    parts = line.split()
    if len(parts) < 2:
        fail("line %d is not 'ssh-<type> <base64> [comment]': %r" % (lineno, line[:60]))
    ktype, blob = parts[0], parts[1]
    if ktype not in ALLOWED:
        fail("line %d: unsupported key type %r (the device's sshd accepts: %s)"
             % (lineno, ktype, ", ".join(ALLOWED)))
    try:
        raw = base64.b64decode(blob, validate=True)
    except Exception:
        fail("line %d: key body is not valid base64" % lineno)
    # SSH wire format: uint32 length + the algorithm name, which must match.
    if len(raw) < 4:
        fail("line %d: key body is truncated" % lineno)
    (n,) = struct.unpack(">I", raw[:4])
    if n > len(raw) - 4 or raw[4:4 + n].decode("ascii", "replace") != ktype:
        fail("line %d: key body does not encode a %s key" % (lineno, ktype))
    if HEREDOC_TAG in line:
        fail("line %d contains the internal heredoc marker %s" % (lineno, HEREDOC_TAG))
    fp = base64.b64encode(hashlib.sha256(raw).digest()).decode().rstrip("=")
    return line, "SHA256:" + fp


def load_keys(path):
    keys = []
    with open(path, "r", errors="replace") as f:
        for lineno, raw in enumerate(f, 1):
            # strip a stray CR (a Windows-authored .pub file) and comments
            line = raw.replace("\r", "").strip()
            if not line or line.startswith("#"):
                continue
            keys.append(parse_key(line, lineno))
    if not keys:
        fail("no public keys found in %s" % path)
    return keys


HOOK_TEMPLATE = """#!/bin/sh
# ============================================================================
#  SSH debug access for the Benro Polaris
#  Added by benro-polaris-firmware-patcher (--ssh-key / -SshKey). NOT stock.
#
#  /app/bootapp (run at boot by /etc/init.d/S10mpp) executes this file if it
#  exists -- it is one of the stock firmware's own optional hooks, so nothing
#  else in the firmware had to be modified to add it.
#
#  The stock rootfs ALREADY starts OpenSSH (/usr/local/bin/sshd, from
#  /etc/init.d/rcS) with PermitRootLogin yes. All this script does is append the
#  public key(s) below to /root/.ssh/authorized_keys, so you can log in with
#  your key instead of the root password:
#
#      ssh -i <your private key> root@<polaris ip>
#
#  It appends only what is missing (existing keys are kept), and never touches
#  passwords, sshd_config, or any other firmware file.
#
#  Authorised key fingerprints:
{fingerprints}
#
#  To remove access: delete this file (/app/{hook_name}) and remove the key
#  from /root/.ssh/authorized_keys on the device -- or reflash stock firmware,
#  which restores both partitions.
#
#  Deliberately NOT `set -e`: this runs during boot and must never be able to
#  abort the rest of bootapp.
# ============================================================================

AK_DIR=/root/.ssh
AK="$AK_DIR/authorized_keys"

mkdir -p "$AK_DIR" 2>/dev/null
[ -f "$AK" ] || : > "$AK"

added=0
while IFS= read -r key; do
    [ -n "$key" ] || continue
    if grep -qxF "$key" "$AK" 2>/dev/null; then
        continue
    fi
    printf '%s\\n' "$key" >> "$AK"
    added=$((added + 1))
done <<'{tag}'
{keys}
{tag}

# sshd (StrictModes is on by default) refuses group/world-writable key files.
chmod 700 "$AK_DIR" 2>/dev/null
chmod 600 "$AK" 2>/dev/null
chown 0:0 "$AK_DIR" "$AK" 2>/dev/null

echo "[polaris-ssh] $AK ready ($added key(s) added this boot)"

# rcS starts sshd immediately after bootapp returns, so normally there is
# nothing to do here. If a firmware ever stops doing that, bring it up
# ourselves -- a no-op while something is already listening.
( sleep 20
  pidof sshd >/dev/null 2>&1 && exit 0
  [ -x /usr/local/bin/sshd ] && /usr/local/bin/sshd
) >/dev/null 2>&1 &

exit 0
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", required=True, help="file of authorized_keys lines")
    ap.add_argument("--hook-name", required=True, help="basename it is installed as in /app")
    ap.add_argument("--out", help="write the hook here (default: stdout)")
    a = ap.parse_args()

    keys = load_keys(a.keys)
    for line, fp in keys:
        comment = " ".join(line.split()[2:]) or "(no comment)"
        sys.stderr.write("[ssh-key] %s  %s  %s\n" % (fp, line.split()[0], comment))

    script = HOOK_TEMPLATE.format(
        hook_name=a.hook_name,
        tag=HEREDOC_TAG,
        fingerprints="\n".join("#    %s  %s" % (fp, k.split()[0]) for k, fp in keys),
        keys="\n".join(k for k, _ in keys),
    )
    if a.out:
        with open(a.out, "w") as f:
            f.write(script)
    else:
        sys.stdout.write(script)


if __name__ == "__main__":
    main()
