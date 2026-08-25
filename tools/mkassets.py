#!/usr/bin/env python3
"""Build the `assets` partition image read by common/xprs_assets.

WHY THIS EXISTS
    A 320x240 RGB565 splash is 153,600 bytes. Compiled into the firmware as
    a C array it is 153 KB of .rodata -- more than the legacy multiboard
    image has left in its whole slot. Put in a flash partition instead it
    costs the app image nothing, and it can be replaced without a reflash.

    On the T-Deck the partition is carved from the 448 KB that were sitting
    unallocated between the coredump partition (ends 0xF90000) and the end
    of the 16 MB chip. Nothing above it moved, so adding it does not erase
    the 11 MB FAT archive.

BYTE ORDER -- READ THIS BEFORE BLAMING THE COLOURS
    xprs_st7789's st7789_flush() does NO byte swapping: it hands the
    buffer straight to spi_device_polling_transmit(), so what is in memory
    is what goes on the wire. LVGL is built here with CONFIG_LV_COLOR_16_SWAP
    unset, which means it produces native little-endian uint16 pixels, and
    that path visibly works on the glass today. So the default here is
    little-endian, to match the one configuration known to be correct.

    Note that st7789_fill_color() swaps its argument, which implies the
    panel wants big-endian. The two cannot both be right. Rather than guess,
    this tool has a flag: if the splash comes out with red and blue looking
    wrong, rebuild with --swap-bytes. It is one flag, not a format change.

USAGE
    tools/mkassets.py -o assets.bin --partition-size 0x30000 \
        splash=art/splash.png icon_lora=art/lora.png

    Names are what the firmware passes to xasset_find(); max 24 bytes.
    A .png/.jpg/.bmp/.gif source becomes an RGB565 image asset (needs
    Pillow); anything else is stored as opaque bytes.

FLASHING
    esptool.py --chip esp32s3 write_flash 0xF90000 assets.bin
"""

import argparse
import os
import struct
import sys

MAGIC = b"XASS"
VERSION = 1
NAME_MAX = 24
HDR_BYTES = 8
ENT_BYTES = 36  # 24 name + 4 off + 4 len + 2 kind + 2 pad
KIND_RGB565 = 1
KIND_RAW = 2

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".gif"}


def to_rgb565(path, swap):
    """Return (w, h, pixel_bytes) for an image file."""
    try:
        from PIL import Image
    except ImportError:
        sys.exit("error: Pillow is needed for image assets "
                 "(pip install Pillow), or supply a pre-converted raw file")

    with Image.open(path) as im:
        im = im.convert("RGB")
        w, h = im.size
        pix = im.tobytes()

    out = bytearray(w * h * 2)
    fmt = ">H" if swap else "<H"
    for i in range(w * h):
        r, g, b = pix[i * 3], pix[i * 3 + 1], pix[i * 3 + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        struct.pack_into(fmt, out, i * 2, v)
    return w, h, bytes(out)


def build(entries, swap):
    """entries: list of (name, path). Returns (archive_bytes, payload_report)."""
    payloads = []
    for name, path in entries:
        if len(name.encode()) > NAME_MAX:
            sys.exit(f"error: asset name '{name}' is longer than {NAME_MAX} bytes")
        ext = os.path.splitext(path)[1].lower()
        if ext in IMAGE_EXT:
            w, h, px = to_rgb565(path, swap)
            blob = struct.pack("<HH", w, h) + px
            payloads.append((name, KIND_RGB565, blob, f"{w}x{h}"))
        else:
            with open(path, "rb") as f:
                blob = f.read()
            payloads.append((name, KIND_RAW, blob, "raw"))

    count = len(payloads)
    off = HDR_BYTES + count * ENT_BYTES
    table = bytearray()
    body = bytearray()
    for name, kind, blob, _ in payloads:
        table += name.encode().ljust(NAME_MAX, b"\0")
        table += struct.pack("<IIHH", off, len(blob), kind, 0)
        body += blob
        off += len(blob)

    return struct.pack("<4sHH", MAGIC, VERSION, count) + bytes(table) + bytes(body), payloads


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", required=True, help="output image")
    ap.add_argument("--partition-size", default="0x30000",
                    help="fail if the archive does not fit (default 0x30000)")
    ap.add_argument("--swap-bytes", action="store_true",
                    help="store pixels big-endian (see the byte-order note above)")
    ap.add_argument("assets", nargs="+", metavar="NAME=FILE")
    args = ap.parse_args()

    entries = []
    for spec in args.assets:
        if "=" not in spec:
            sys.exit(f"error: expected NAME=FILE, got '{spec}'")
        name, path = spec.split("=", 1)
        if not os.path.isfile(path):
            sys.exit(f"error: no such file: {path}")
        entries.append((name, path))

    blob, payloads = build(entries, args.swap_bytes)
    limit = int(args.partition_size, 0)
    if len(blob) > limit:
        sys.exit(f"error: archive is {len(blob):,} bytes, partition is {limit:,}")

    with open(args.out, "wb") as f:
        f.write(blob)

    for name, kind, data, note in payloads:
        print(f"  {name:<24s} {note:>10s} {len(data):>9,} B")
    pct = 100.0 * len(blob) / limit
    print(f"  {'':<24s} {'TOTAL':>10s} {len(blob):>9,} B "
          f"({pct:.1f}% of {limit:,}) -> {args.out}")
    print(f"  byte order: "
          f"{'big-endian (--swap-bytes)' if args.swap_bytes else 'little-endian (LVGL default)'}")


if __name__ == "__main__":
    main()
