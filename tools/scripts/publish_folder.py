#!/usr/bin/env python3
"""Publish the released images as the project's firmware folder.

    tools/scripts/publish_folder.py [--channel stable] [--api http://127.0.0.1:3456]

The last step of a release (tools/build-published.sh runs it). It stages
every shipping board's prebuilt image as dist/<channel>/<board>-<version>.bin
and asks the XPRS app running on this machine -- the maintainer's, whose
profile key is the project's firmware publisher (common/xprs_ota/
fw_publisher.h) -- to publish that directory as a folder the format can find
(XPRS.md 7.7.3): it serves the images by digest over Reticulum and Bluetooth,
and signs and deposits the folder and member descriptions. The signed wires
come back and are written to docs/firmware.json beside the images' digests and
URLs, so every other XPRS app downloads the same images from GitHub, verifies
them against the same signatures, keeps them and relays the descriptions:
from then on a station finds the release through any archiver and fetches it
from any app near it, and nobody is asked to do anything.

The app must be running with the remote API on (Settings > Remote control
API). Without it the release is incomplete, and this says so and fails.
"""
import json
import os
import shutil
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RAW_URL = "https://raw.githubusercontent.com/xprs-dev/firmware/main"


CHANNEL_DIR = {"stable": "prebuilt", "beta": "prebuilt-beta"}


def sha256_of(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    channel, api = "stable", os.environ.get("XPRS_API", "http://127.0.0.1:3456")
    it = iter(argv)
    for a in it:
        if a == "--channel":
            channel = next(it)
        elif a == "--api":
            api = next(it)
    sub = CHANNEL_DIR.get(channel)
    if not sub:
        sys.exit(f"publish_folder: unknown channel {channel}")
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
    body = json.dumps({"name": f"XPRS firmware {channel}", "tag": channel, "dir": stage}).encode()
    req = urllib.request.Request(f"{api}/api/xprs/folder/publish", data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            resp = json.load(r)
    except Exception as e:  # noqa: BLE001
        sys.exit(f"publish_folder: the XPRS app at {api} did not publish the folder ({e}); "
                 "start it with the remote API on and run this again")
    if not resp.get("ok") or not resp.get("wires"):
        sys.exit(f"publish_folder: the app answered {resp}")
    out_path = os.path.join(ROOT, "docs", "firmware.json")
    out = json.load(open(out_path)) if os.path.isfile(out_path) else {}
    out["generated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    out["publisher"] = {"callsign": resp.get("self"), "key": resp.get("key")}
    out[channel] = {"listing": resp.get("listing"), "wires": resp["wires"], "members": members}
    json.dump(out, open(out_path, "w"), indent=2)
    print(f"publish_folder: {channel}: {len(members)} images, {len(resp['wires'])} signed "
          f"descriptions from {resp.get('self')} -> docs/firmware.json")


if __name__ == "__main__":
    main(sys.argv[1:])
