#!/bin/sh
# Round-trip: build an archive with the tool, read it with the firmware code.
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# An 8x4 image whose first pixel is pure red, and a 5-byte raw blob.
python3 - "$OUT" <<'PY'
import sys, struct, zlib, os
d = sys.argv[1]
w, h = 8, 4
rows = b"".join(b"\x00" + b"\xff\x00\x00" * w for _ in range(h))
def chunk(t, b):
    c = t + b
    return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(rows))
       + chunk(b"IEND", b""))
open(os.path.join(d, "splash.png"), "wb").write(png)
open(os.path.join(d, "note.txt"), "wb").write(b"hello")
PY

python3 ../../../tools/mkassets.py -o "$OUT/assets.bin" \
    --partition-size 0x30000 \
    "splash=$OUT/splash.png" "note=$OUT/note.txt"

gcc -std=gnu99 -Wall -Wextra -Werror -O1 -I. -I.. -Istubs \
    -o "$OUT/test_xasset" test_xasset_host.c ../xasset.c
"$OUT/test_xasset" "$OUT/assets.bin"
