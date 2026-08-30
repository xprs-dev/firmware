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

TOP = ["id", "name", "vendor", "sku", "product_url", "summary", "status",
       "silicon", "bearers", "io", "physical", "firmware", "docs", "images"]
STATUS = {"shipping", "legacy", "planned", "unsupported"}
FAMILY = {"esp32", "esp32s3", "esp32c3", "nrf52"}
BEARERS = ["ble5", "lora", "lan", "espnow"]
# yes/no/untested, as STRINGS. Unquoted yes and no are booleans in YAML and
# `untested` is not, which would make one field two types.
VERDICT = {"yes", "no", "untested"}
SILICON = ["mcu", "family", "core", "ram_kb", "psram_mb", "flash_mb"]
FIRMWARE = ["toolchain", "project", "env", "version", "artifact", "flashing"]

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

    fw = d.get("firmware") or {}
    for k in FIRMWARE:
        if k not in fw:
            bad(f"firmware is missing '{k}'")
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
