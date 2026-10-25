#!/usr/bin/env python3
"""Publish the released images as the project's firmware folder, signed here.

    XPRS_PUBLISHER_NSEC=nsec1... tools/scripts/publish_folder.py [--channel stable]

The last step of a release: the release workflow runs it, and so does
tools/build-published.sh on a bench. It stages every shipping board's
prebuilt image as dist/<channel>/<board>-<version>.bin, writes the folder's
XFL1 listing (XPRS.md 7.7.2), and signs the folder description and one
member description per image (7.7.1, 7.7.3) with the project's firmware
publisher key -- the key every published station pins
(common/xprs_ota/fw_publisher.h). The signatures are made by tools/xsign.c,
the same C code the station verifies with. The signed wires go to
docs/firmware.json beside the images' digests and URLs: every XPRS app reads
that document, downloads and verifies the images, keeps them, serves them
by digest over Reticulum and Bluetooth and relays the descriptions, so a
station finds the release through any archiver and fetches it from any app
near it. Nobody is asked to do anything.

The key comes from the environment (XPRS_PUBLISHER_NSEC, an nsec or 64 hex
characters) and is never written anywhere. It must be the key
fw_publisher.h names, or the stations would refuse what it signs; a
different key fails the publish here instead.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import base64

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RAW_URL = "https://raw.githubusercontent.com/xprs-dev/firmware/main"
CHANNEL_DIR = {"stable": "prebuilt", "beta": "prebuilt-beta"}
PACKET_MAX = 250
B32 = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def bech32_decode(s):
    """The data part of a bech32 string (BIP-173), as bytes."""
    s = s.strip().lower()
    hrp, data = s.rsplit("1", 1)
    vals = [B32.index(c) for c in data[:-6]]
    acc = bits = 0
    out = bytearray()
    for v in vals:
        acc = (acc << 5) | v
        bits += 5
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xFF)
    return hrp, bytes(out)


def bech32_encode(hrp, payload):
    def polymod(values):
        gen = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
        chk = 1
        for v in values:
            b = chk >> 25
            chk = ((chk & 0x1FFFFFF) << 5) ^ v
            for i in range(5):
                chk ^= gen[i] if ((b >> i) & 1) else 0
        return chk
    acc = bits = 0
    data = []
    for b in payload:
        acc = (acc << 8) | b
        bits += 8
        while bits >= 5:
            bits -= 5
            data.append((acc >> bits) & 31)
    if bits:
        data.append((acc << (5 - bits)) & 31)
    hrpx = [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp]
    pm = polymod(hrpx + data + [0] * 6) ^ 1
    chk = [(pm >> 5 * (5 - i)) & 31 for i in range(6)]
    return hrp + "1" + "".join(B32[d] for d in data + chk)


def private_scalar():
    k = os.environ.get("XPRS_PUBLISHER_NSEC", "").strip()
    if not k:
        sys.exit("publish_folder: XPRS_PUBLISHER_NSEC is not set; nothing can be signed")
    if k.startswith("nsec1"):
        hrp, raw = bech32_decode(k)
        if hrp != "nsec" or len(raw) != 32:
            sys.exit("publish_folder: the nsec does not decode")
        return raw.hex()
    if len(k) == 64:
        return k.lower()
    sys.exit("publish_folder: the key is neither an nsec nor 64 hex characters")


def xsign_binary():
    """tools/xsign built from the station's own signer, on demand."""
    out = os.path.join(ROOT, "dist", "xsign")
    src = [os.path.join(ROOT, "tools", "xsign.c"),
           os.path.join(ROOT, "common", "xprs_sig", "xprssig.c")]
    if not os.path.isfile(out) or any(os.path.getmtime(s) > os.path.getmtime(out) for s in src):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        subprocess.run(["gcc", "-O2", "-DXPRSSIG_HOST_TEST",
                        "-I" + os.path.join(ROOT, "common", "xprs_sig"),
                        "-o", out] + src + ["-lcrypto"], check=True)
    return out


def pinned_key():
    h = open(os.path.join(ROOT, "common", "xprs_ota", "fw_publisher.h")).read()
    for line in h.splitlines():
        if "XPRS_FW_PUBLISHER_KEY" in line and '"' in line:
            return line.split('"')[1].lower()
    sys.exit("publish_folder: no XPRS_FW_PUBLISHER_KEY in fw_publisher.h")


def file_ref(sha_hex, ext):
    b = base64.urlsafe_b64encode(bytes.fromhex(sha_hex)).decode().rstrip("=")
    return f"{b}.{ext}" if ext else b


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def sign(xsign, priv, line):
    """`sig:` for a packet: over the packet without sig:/via: (XPRS.md 9.1),
    placed before m: because m: stays last (section 4)."""
    sig = subprocess.run([xsign, priv, line], capture_output=True, text=True, check=True).stdout.strip()
    if " m:" in line:
        head, m = line.split(" m:", 1)
        wire = f"{head} sig:{sig} m:{m}"
    else:
        wire = f"{line} sig:{sig}"
    if len(wire.encode()) > PACKET_MAX:
        sys.exit(f"publish_folder: a description does not fit a packet: {wire}")
    return wire


def main(argv):
    channel = "stable"
    it = iter(argv)
    for a in it:
        if a == "--channel":
            channel = next(it)
    sub = CHANNEL_DIR.get(channel)
    if not sub:
        sys.exit(f"publish_folder: unknown channel {channel}")

    priv = private_scalar()
    xsign = xsign_binary()
    pub = subprocess.run([xsign, "--pub", priv], capture_output=True, text=True, check=True).stdout.strip()
    pinned = pinned_key()
    if pub != pinned:
        sys.exit("publish_folder: the key given is not the firmware publisher the stations pin "
                 f"(fw_publisher.h says {pinned[:8]}..., this key is {pub[:8]}...)")
    npub = bech32_encode("npub", bytes.fromhex(pub))
    self_call = "X1" + npub[5:9].upper()

    stage = os.path.join(ROOT, "dist", channel)
    os.makedirs(stage, exist_ok=True)
    for old in os.listdir(stage):
        os.remove(os.path.join(stage, old))
    members = []
    for bid in sorted(os.listdir(os.path.join(ROOT, "models"))):
        pre = os.path.join(ROOT, "models", bid, sub)
        img, man = os.path.join(pre, "firmware.bin"), os.path.join(pre, "manifest.json")
        if not (os.path.isfile(img) and os.path.isfile(man)):
            continue
        version = json.load(open(man)).get("version") or ""
        if not version or version == "unknown":
            continue
        name = f"{bid}-{version}.bin"
        shutil.copy2(img, os.path.join(stage, name))
        members.append({"board": bid, "version": version, "name": name,
                        "sha256": sha256_of(img), "size": os.path.getsize(img),
                        "url": f"{RAW_URL}/models/{bid}/{sub}/firmware.bin"})
    if not members:
        sys.exit(f"publish_folder: no board has an image in models/*/{sub}")

    # The listing (7.7.2): XFL1, then `ref size name` sorted bytewise by name.
    rows = sorted(members, key=lambda m: m["name"].encode())
    listing = "XFL1\n" + "".join(f"{file_ref(m['sha256'], 'bin')} {m['size']} {m['name']}\n" for m in rows)
    listing_sha = hashlib.sha256(listing.encode()).hexdigest()
    with open(os.path.join(stage, "listing.xfl"), "w") as f:
        f.write(listing)

    ts = time.strftime("%Y-%m-%d_%H:%M:%S", time.gmtime())
    total = sum(m["size"] for m in members)
    wires = [sign(xsign, priv,
                  f"t:file f:{self_call} ts:{ts} file:{file_ref(listing_sha, 'xfl')} kind:folder "
                  f"count:{len(members)} size:{total} m:XPRS firmware {channel}")]
    for m in rows:
        wires.append(sign(xsign, priv,
                          f"t:file f:{self_call} ts:{ts} file:{file_ref(m['sha256'], 'bin')} "
                          f"size:{m['size']} tag:{channel} name:{m['name']}"))

    # Read back what was signed with the verifier the station uses.
    for w in wires:
        head = w.split(" m:", 1)
        toks = [t for t in head[0].split(" ") if not t.startswith("sig:")]
        line = " ".join(toks) + (" m:" + head[1] if len(head) > 1 else "")
        sig = [t for t in head[0].split(" ") if t.startswith("sig:")][0][4:]
        r = subprocess.run([xsign, "--verify", pub, line, sig], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"publish_folder: a signature did not verify: {w}")

    out_path = os.path.join(ROOT, "docs", "firmware.json")
    out = json.load(open(out_path)) if os.path.isfile(out_path) else {}
    out["generated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    out["publisher"] = {"callsign": self_call, "key": pub}
    out[channel] = {"listing": listing_sha, "listing_text": listing, "wires": wires, "members": members}
    json.dump(out, open(out_path, "w"), indent=2)
    print(f"publish_folder: {channel}: {len(members)} images, {len(wires)} signed "
          f"descriptions by {self_call} -> docs/firmware.json")


if __name__ == "__main__":
    main(sys.argv[1:])
