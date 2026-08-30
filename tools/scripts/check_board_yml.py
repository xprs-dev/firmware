#!/usr/bin/env python3
"""Check every models/*/board.yml against the schema in docs/catalog.md.

A catalogue page is only worth building on top of files that are all the
same shape, and the way that stops being true is one board gaining a key
nobody else has, or a bearer written `yes` in one file and `true` in the
next. Both are invisible until a page renders nothing for one board.

    tools/scripts/check_board_yml.py

Exits non-zero on the first shape problem. Says nothing when all is well.
"""
import sys
import glob
import os

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is needed: pip install pyyaml")

TOP = ["id", "name", "vendor", "sku", "product_url", "manual_url", "tagline", "summary",
       "status", "silicon", "radios", "bearers", "xprs", "io", "physical",
       "firmware", "docs", "images", "screenshots"]
STATUS = {"shipping", "legacy", "planned", "unsupported"}
FAMILY = {"esp32", "esp32s3", "esp32c3", "nrf52"}
BEARERS = ["ble5", "lora", "lan", "espnow"]
# yes/no/untested, as STRINGS. Unquoted yes and no are booleans in YAML and
# `untested` is not, which would make one field two types.
VERDICT = {"yes", "no", "untested"}
SILICON = ["mcu", "family", "core", "ram_kb", "psram_mb", "flash_mb"]
RADIOS = ["bluetooth", "wifi", "lora", "other"]
XPRS = ["beacon", "digipeater", "bridge", "igate", "hotspot", "api", "indexer",
        "share", "reticulum", "ota", "gossip", "dashboard", "chat",
        "mesh_session", "vhf"]
XPRS_VERDICT = {"yes", "no", "planned", "untested"}
FIRMWARE = ["toolchain", "project", "env", "version", "artifact", "flash_port", "flashing"]
FLASH_PORT = {"usb-serial", "native-usb", "uf2", None}

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
problems = []


def check(path, d, is_template):
    def bad(msg):
        problems.append(f"{os.path.relpath(path, root)}: {msg}")

    for k in TOP:
        if k not in d:
            bad(f"missing top-level key '{k}'")
    for k in d:
        if k not in TOP:
            bad(f"unknown top-level key '{k}'")

    if is_template:
        # The template is the empty shape: keys present, values absent. That
        # is the whole point of it, so the value rules below do not apply.
        return

    if d.get("id") != os.path.basename(os.path.dirname(path)):
        bad(f"id '{d.get('id')}' does not match the folder name")
    if d.get("status") not in STATUS:
        bad(f"status '{d.get('status')}' is not one of {sorted(STATUS)}")

    sil = d.get("silicon") or {}
    for k in SILICON:
        if k not in sil:
            bad(f"silicon is missing '{k}'")
    if sil.get("family") not in FAMILY:
        bad(f"silicon.family '{sil.get('family')}' is not one of {sorted(FAMILY)}")

    bea = d.get("bearers") or {}
    for k in BEARERS:
        v = bea.get(k)
        if v not in VERDICT:
            bad(f"bearers.{k} is {v!r}; want one of {sorted(VERDICT)} as a "
                f"quoted string (bare yes/no are YAML booleans)")
        # A bearer that is anything but a plain yes owes a reason.
        if v in ("no", "untested") and not (bea.get(k + "_note") or "").strip():
            bad(f"bearers.{k} is '{v}' with no {k}_note saying why")

    rad = d.get("radios") or {}
    for k in RADIOS:
        if k not in rad:
            bad(f"radios is missing '{k}'")

    xp = d.get("xprs") or {}
    for k in XPRS:
        v = xp.get(k)
        if v not in XPRS_VERDICT:
            bad(f"xprs.{k} is {v!r}; want one of {sorted(XPRS_VERDICT)} as a "
                f"quoted string")
        elif v != "yes" and not (xp.get(k + "_note") or "").strip():
            bad(f"xprs.{k} is '{v}' with no {k}_note saying why")
    for k in xp:
        base = k[:-5] if k.endswith("_note") else k
        if base not in XPRS and k != "note":
            bad(f"xprs has unknown role '{k}'")

    fw = d.get("firmware") or {}
    for k in FIRMWARE:
        if k not in fw:
            bad(f"firmware is missing '{k}'")
    if fw.get("flash_port") not in FLASH_PORT:
        bad(f"firmware.flash_port {fw.get('flash_port')!r} is not one of usb-serial | native-usb | uf2")
    proj = fw.get("project")
    if proj and not os.path.isdir(os.path.join(root, proj)):
        bad(f"firmware.project '{proj}' does not exist")
    if d.get("status") == "shipping" and not proj:
        bad("status is 'shipping' but firmware.project is empty")

    for i, img in enumerate(d.get("images") or []):
        for k in ("file", "caption", "credit"):
            if not (img.get(k) or "").strip():
                bad(f"images[{i}] has no '{k}' "
                    f"(docs/catalog.md: every image carries a credit)")
        f = img.get("file")
        if f and not os.path.isfile(os.path.join(os.path.dirname(path), f)):
            bad(f"images[{i}] file '{f}' does not exist")

    for i, sc in enumerate(d.get("screenshots") or []):
        for k in ("file", "caption"):
            if not (sc.get(k) or "").strip():
                bad(f"screenshots[{i}] has no '{k}'")
        f = sc.get("file")
        if f and not os.path.isfile(os.path.join(os.path.dirname(path), f)):
            bad(f"screenshots[{i}] file '{f}' does not exist")


paths = sorted(glob.glob(os.path.join(root, "models", "*", "board.yml")))
if not paths:
    sys.exit("no models/*/board.yml found")

for p in paths:
    try:
        d = yaml.safe_load(open(p)) or {}
    except yaml.YAMLError as e:
        problems.append(f"{os.path.relpath(p, root)}: will not parse: {e}")
        continue
    check(p, d, is_template=os.path.basename(os.path.dirname(p)) == "_template")

if problems:
    for m in problems:
        print(m, file=sys.stderr)
    sys.exit(f"\n{len(problems)} problem(s) in {len(paths)} board file(s)")
print(f"{len(paths)} board files, all well")
