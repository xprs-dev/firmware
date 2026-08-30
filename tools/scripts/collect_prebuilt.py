#!/usr/bin/env python3
"""Copy each board's built image into models/<id>/prebuilt/ and write the
manifest the web flasher reads.

    tools/scripts/collect_prebuilt.py [id ...]

Reads firmware.project / firmware.env / silicon.family from board.yml, takes
what `pio run` left in <project>/.pio/build/<env>/, and produces:

  ESP32 family   bootloader.bin, partitions.bin, firmware.bin, manifest.json
                 (ESP Web Tools format; offsets from the chip and the
                 project's partitions.csv, app at the first ota_0/factory)
  nRF52          firmware.uf2 (from firmware.hex, family 0xADA52840) -- copied
                 onto the bootloader's mass-storage volume, no flasher needed

A board whose project has not been built is skipped and said so. Nothing
here builds anything: the image in prebuilt/ is whatever was last built,
and version.txt travels with it so the page can say which.
"""
import os
import sys
import glob
import json
import shutil
import subprocess

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is needed: pip install pyyaml")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BOOT_OFFSET = {"esp32": 0x1000, "esp32s3": 0x0, "esp32c3": 0x0}
CHIP_FAMILY = {"esp32": "ESP32", "esp32s3": "ESP32-S3", "esp32c3": "ESP32-C3"}
UF2CONV = glob.glob(os.path.expanduser(
    "~/.platformio/packages/framework-arduinoadafruitnrf52/tools/uf2conv/uf2conv.py"))


def app_offset(csv_path):
    for line in open(csv_path):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(",")]
        if len(f) >= 4 and f[1] == "app" and f[2] in ("ota_0", "factory"):
            return int(f[3], 0)
    return 0x10000


def collect(b):
    fw, sil = b.get("firmware") or {}, b.get("silicon") or {}
    proj, env, fam = fw.get("project"), fw.get("env"), sil.get("family")
    if not proj or not env:
        return f"{b['id']}: no buildable project"
    build = os.path.join(ROOT, proj, ".pio", "build", env)
    out = os.path.join(ROOT, "models", b["id"], "prebuilt")
    if not os.path.isdir(build):
        return f"{b['id']}: not built ({os.path.relpath(build, ROOT)} missing)"
    version = None
    vt = os.path.join(ROOT, proj, "version.txt")
    if os.path.isfile(vt):
        version = open(vt).read().strip()

    if fam in CHIP_FAMILY:
        parts = [("bootloader.bin", BOOT_OFFSET[fam]), ("partitions.bin", 0x8000),
                 ("firmware.bin", app_offset(os.path.join(ROOT, proj, "partitions.csv")))]
        for name, _ in parts:
            if not os.path.isfile(os.path.join(build, name)):
                return f"{b['id']}: {name} missing in build dir"
        os.makedirs(out, exist_ok=True)
        for name, _ in parts:
            shutil.copy2(os.path.join(build, name), os.path.join(out, name))
        manifest = {
            "name": f"XPRS {b.get('name')}",
            "version": version or "unknown",
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": CHIP_FAMILY[fam],
                        "parts": [{"path": n, "offset": o} for n, o in parts]}],
        }
        json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
        return f"{b['id']}: {', '.join(n for n, _ in parts)} + manifest.json (v{version})"

    if fam == "nrf52":
        hexf = os.path.join(build, "firmware.hex")
        if not os.path.isfile(hexf):
            return f"{b['id']}: firmware.hex missing in build dir"
        if not UF2CONV:
            return f"{b['id']}: uf2conv.py not found in the PlatformIO packages"
        os.makedirs(out, exist_ok=True)
        subprocess.run([sys.executable, UF2CONV[0], "-f", "0xADA52840", "-c",
                        "-o", os.path.join(out, "firmware.uf2"), hexf],
                       check=True, stdout=subprocess.DEVNULL)
        return f"{b['id']}: firmware.uf2 (v{version})"

    return f"{b['id']}: family {fam!r} not handled"


if __name__ == "__main__":
    want = set(sys.argv[1:])
    for p in sorted(glob.glob(os.path.join(ROOT, "models", "*", "board.yml"))):
        bid = os.path.basename(os.path.dirname(p))
        if bid == "_template" or (want and bid not in want):
            continue
        b = yaml.safe_load(open(p)) or {}
        b.setdefault("id", bid)
        print(collect(b))
