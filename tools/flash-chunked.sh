#!/usr/bin/env bash
# Flash an app image in pieces.
#
# The T-Deck talks over the ESP32-S3's native USB-serial-JTAG, and a single
# write of a ~1.4 MB image reliably dies part-way through with
#
#     A serial exception error occurred: Could not configure port:
#     (5, 'Input/output error')
#
# leaving an invalid image and a board that reboot-loops in the bootloader --
# which is worse than not having flashed at all, because the loop makes the
# USB device churn and the next attempt harder. Splitting the write into
# 256 KB pieces, each its own esptool invocation with its own reset, works;
# an individual piece that fails is simply retried.
#
# Usage: tools/flash-chunked.sh <port> <image.bin> [offset]
set -u
PORT=${1:?usage: flash-chunked.sh <port> <image.bin> [offset]}
IMG=${2:?usage: flash-chunked.sh <port> <image.bin> [offset]}
BASE=${3:-0x10000}
CHUNK=$((256 * 1024))
ET=~/.platformio/packages/tool-esptoolpy/esptool.py
PY=~/.platformio/penv/bin/python
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

split -b "$CHUNK" -d "$IMG" "$TMP/part_"
rc=0
for f in "$TMP"/part_*; do
    i=${f##*part_}
    off=$(( BASE + 10#$i * CHUNK ))
    ok=0
    for try in 1 2 3 4; do
        if $PY "$ET" --chip esp32s3 --port "$PORT" --baud 460800 \
             --before default_reset --after no_reset \
             write_flash "$(printf 0x%x $off)" "$f" > "$TMP/log" 2>&1; then
            ok=1; break
        fi
        echo "  chunk $i attempt $try failed: $(grep -m1 -oE 'A serial exception.*|A fatal error.*' "$TMP/log")"
        sleep 2
    done
    if [ $ok = 1 ]; then echo "chunk $i @ $(printf 0x%x $off) ok"
    else echo "chunk $i @ $(printf 0x%x $off) FAILED -- image is incomplete"; rc=1; fi
done
$PY "$ET" --chip esp32s3 --port "$PORT" --before default_reset --after hard_reset \
    chip_id > /dev/null 2>&1
exit $rc
