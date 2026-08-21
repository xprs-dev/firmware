#!/usr/bin/env python3
"""Pack Wrench scripts into the signed XSCB bundle a station runs.

WHY IT IS TWO STEPS
    Building the container and signing it are separated on purpose. The
    signing key is a station's whole trust anchor -- whoever holds it can put
    code on every board that trusts it -- and tools/sign_firmware.dart already
    has the one implementation of that, with the doctrine that the key never
    enters CI. Reimplementing nostr signing here would give the fleet a second
    thing that has to agree, and a second thing that can be wrong.

    So:

      1. build   -- pack the modules, print the line that must be signed
      2. sign    -- with the existing Dart tool, offline, by a human
      3. stamp   -- write the 60-character signature into the header

USAGE
    tools/mkbundle.py build --board tdeck --id panels --version 0.1.0 \
        --out panels.xscb --types t:message,t:report \
        chat=scripts/chat.w radar=scripts/radar.w:1000

    (a module may carry a tick period in ms after a colon; 0 or absent means
    it wants no tick. The floor is 100 ms and the device re-clamps it.)

    tools/mkbundle.py stamp --bundle panels.xscb --sig <60 base85 chars>

    tools/mkbundle.py show  --bundle panels.xscb

The format is documented once, in common/xprs_script/xs_bundle.h, and checked
against the device parser by common/xprs_script/test/test_bundle_host.sh.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = b"XSCB"
FORMAT = 1
ID_MAX, VER_MAX, NAME_MAX, SIG_MAX = 16, 24, 24, 64
MODS_MAX, TYPES_MAX, TYPE_LEN = 8, 8, 16
BODY_OFF, MOD_ENTRY = 120, 36


def fixed(s, n, what):
    b = s.encode()
    if len(b) > n:
        sys.exit(f"error: {what} '{s}' is longer than {n} bytes")
    return b.ljust(n, b"\0")


def compile_module(src):
    """Run tools/wrenchc over one .w source, return the bytecode."""
    wrenchc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wrenchc")
    if not os.path.isfile(wrenchc):
        sys.exit("error: tools/wrenchc is not built -- run tools/build_wrenchc.sh")
    with tempfile.NamedTemporaryFile(suffix=".wrb", delete=False) as t:
        out = t.name
    try:
        r = subprocess.run([wrenchc, src, out], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"error: {r.stderr.strip() or 'wrenchc failed on ' + src}")
        with open(out, "rb") as f:
            return f.read()
    finally:
        os.unlink(out)


def build(args):
    mods = []
    for spec in args.modules:
        if "=" not in spec:
            sys.exit(f"error: expected NAME=FILE[:tick_ms], got '{spec}'")
        name, rest = spec.split("=", 1)
        tick = 0
        if ":" in rest:
            rest, t = rest.rsplit(":", 1)
            tick = int(t)
        if not os.path.isfile(rest):
            sys.exit(f"error: no such file: {rest}")
        mods.append((name, rest, tick))

    if not mods or len(mods) > MODS_MAX:
        sys.exit(f"error: 1..{MODS_MAX} modules, got {len(mods)}")

    types = [t.strip() for t in args.types.split(",") if t.strip()] if args.types else []
    if len(types) > TYPES_MAX:
        sys.exit(f"error: at most {TYPES_MAX} packet types")

    blobs = [(n, compile_module(p), tick) for n, p, tick in mods]

    table_bytes = len(blobs) * MOD_ENTRY
    types_bytes = TYPES_MAX * TYPE_LEN
    off = BODY_OFF + table_bytes + types_bytes

    table, body = bytearray(), bytearray()
    for name, blob, tick in blobs:
        table += fixed(name, NAME_MAX, "module name")
        table += struct.pack("<IIHH", off, len(blob), tick, 0)
        body += blob
        off += len(blob)

    tblock = b"".join(fixed(t, TYPE_LEN, "packet type") for t in types)
    tblock = tblock.ljust(types_bytes, b"\0")

    signed = bytes(table) + tblock + bytes(body)
    header = (MAGIC + struct.pack("<HH", FORMAT, len(blobs))
              + fixed(args.id, ID_MAX, "bundle id")
              + fixed(args.version, VER_MAX, "version")
              + b"\0" * SIG_MAX                       # stamped in later
              + struct.pack("<II", len(signed), 0))
    assert len(header) == BODY_OFF, len(header)

    with open(args.out, "wb") as f:
        f.write(header + signed)

    sha = hashlib.sha256(signed).hexdigest()
    line = f"xprsscr1 {args.board} {args.id} {args.version} {len(signed)} {sha}"

    for name, blob, tick in blobs:
        print(f"  {name:<24s} {len(blob):>6,} B  tick {tick or '-'}")
    print(f"  {'':<24s} {len(header) + len(signed):>6,} B total -> {args.out}")
    if types:
        print(f"  packet types: {', '.join(types)}")
    print()
    print("Sign this line, exactly, then stamp the result in:")
    print()
    print(f"    {line}")
    print()
    print("    dart run tools/sign_firmware.dart --line '<the line above>' "
          "--nsec-file <key>")
    print(f"    tools/mkbundle.py stamp --bundle {args.out} --sig <60 chars>")


def stamp(args):
    with open(args.bundle, "rb") as f:
        data = bytearray(f.read())
    if bytes(data[:4]) != MAGIC:
        sys.exit("error: not an XSCB bundle")
    if len(args.sig) != 60:
        sys.exit(f"error: signature is {len(args.sig)} characters, expected 60")
    data[48:48 + SIG_MAX] = fixed(args.sig, SIG_MAX, "signature")
    with open(args.bundle, "wb") as f:
        f.write(data)
    print(f"  stamped {args.bundle}")


def show(args):
    with open(args.bundle, "rb") as f:
        data = f.read()
    if bytes(data[:4]) != MAGIC:
        sys.exit("error: not an XSCB bundle")
    fmt, nmod = struct.unpack("<HH", data[4:8])
    bid = data[8:24].rstrip(b"\0").decode()
    ver = data[24:48].rstrip(b"\0").decode()
    sig = data[48:112].rstrip(b"\0").decode()
    slen, _ = struct.unpack("<II", data[112:120])
    sha = hashlib.sha256(data[BODY_OFF:BODY_OFF + slen]).hexdigest()
    print(f"  format {fmt}  id '{bid}'  version '{ver}'  modules {nmod}")
    print(f"  signed {slen} bytes, sha256 {sha}")
    print(f"  signature {'(unsigned)' if not sig else sig}")
    for i in range(nmod):
        e = BODY_OFF + i * MOD_ENTRY
        name = data[e:e + NAME_MAX].rstrip(b"\0").decode()
        off, ln, tick, _ = struct.unpack("<IIHH", data[e + 24:e + MOD_ENTRY])
        print(f"    {name:<24s} {ln:>6,} B at {off}  tick {tick or '-'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--board", required=True)
    b.add_argument("--id", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--types", default="")
    b.add_argument("modules", nargs="+", metavar="NAME=FILE[:tick_ms]")
    b.set_defaults(fn=build)

    s = sub.add_parser("stamp")
    s.add_argument("--bundle", required=True)
    s.add_argument("--sig", required=True)
    s.set_defaults(fn=stamp)

    w = sub.add_parser("show")
    w.add_argument("--bundle", required=True)
    w.set_defaults(fn=show)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
