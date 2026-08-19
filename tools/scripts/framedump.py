#!/usr/bin/env python3
"""Capture a screenshot from a running board over serial.

The firmware mirrors one full LVGL refresh onto the UART as base64 when
asked (m5stack: the 'S' serial key; t-dongle: the 'dump' console command).
Wire format, produced by xprs_ui.c / xprs_ui_mini.c:

    FRAMEDUMP BEGIN <w> <h>
    SLICE <x1> <y1> <x2> <y2> <b64len>
    <base64 lines...>            (RGB565 big-endian, slice pixels)
    ...more slices...
    FRAMEDUMP END

Usage:
    framedump.py [--port /dev/ttyUSB0] [--baud 115200]
                 [--cmd S | --cmd "dump\\n"] [--boot-wait 45] out.png

Opening a CP2104 port resets the board; --boot-wait covers the reboot
before the trigger is sent. USB-JTAG consoles (the S3) do not reset.
"""
import argparse
import base64
import sys
import time

import serial               # pyserial
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cmd", default="S")
    ap.add_argument("--boot-wait", type=float, default=0)
    ap.add_argument("--timeout", type=float, default=30)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=2)
    if args.boot_wait:
        print(f"waiting {args.boot_wait}s for the board to boot...")
        t0 = time.time()
        while time.time() - t0 < args.boot_wait:
            ser.read(4096)
    ser.reset_input_buffer()
    ser.write(args.cmd.replace("\\n", "\n").encode())
    ser.flush()

    img = None
    w = h = 0
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        line = ser.readline().decode("ascii", "ignore").strip()
        if not line:
            continue
        if line.startswith("FRAMEDUMP BEGIN"):
            _, _, ws, hs = line.split()
            w, h = int(ws), int(hs)
            img = Image.new("RGB", (w, h), "black")
            print(f"frame {w}x{h}")
        elif line.startswith("SLICE") and img is not None:
            _, x1, y1, x2, y2, blen = line.split()
            x1, y1, x2, y2, blen = map(int, (x1, y1, x2, y2, blen))
            b64 = []
            got = 0
            while got < blen and time.time() < deadline:
                l2 = ser.readline().decode("ascii", "ignore").strip()
                b64.append(l2)
                got += len(l2)
            raw = base64.b64decode("".join(b64))
            sw = x2 - x1 + 1
            for i in range(0, len(raw) // 2):
                v = (raw[2 * i] << 8) | raw[2 * i + 1]   # big-endian RGB565
                r = (v >> 11) << 3
                g = ((v >> 5) & 0x3F) << 2
                b = (v & 0x1F) << 3
                img.putpixel((x1 + i % sw, y1 + i // sw), (r, g, b))
        elif line.startswith("FRAMEDUMP END") and img is not None:
            img.save(args.out)
            print(f"saved {args.out}")
            return 0
    print("timed out without a complete frame", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
