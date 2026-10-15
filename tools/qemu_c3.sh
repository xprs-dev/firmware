#!/bin/bash
# Run the ESP32-C3 station under Espressif's QEMU (machine esp32c3), for
# measuring stacks and heap without the board. See docs/esp32.md, "The C3
# under QEMU".
#
#   tools/qemu_c3.sh build            build c3qemu, write a fresh 4 MB image
#   tools/qemu_c3.sh run [SECONDS]    boot it; console in qemu-c3.log
#   tools/qemu_c3.sh console [SECONDS] boot it with the console on tcp:5555
#                                     (type `cfg ...` lines into it)
#   tools/qemu_c3.sh update           put a new build into the image, keeping
#                                     its NVS (owner, rns_hub, ...)
#
# The API answers on http://127.0.0.1:18080 (emulated OpenCores Ethernet,
# QEMU user networking, which also reaches the internet). QEMU has no WiFi,
# BLE or ESP-NOW. Install QEMU once with:
#   IDF_PATH=~/.platformio/packages/framework-espidf \
#     ~/.espressif/python_env/idf5.5_py3.12_env/bin/python \
#     ~/.platformio/packages/framework-espidf/tools/idf_tools.py install qemu-riscv32
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$ROOT/models/esp32c3-mini/firmware"
OUT="${QEMU_C3_DIR:-$PROJ/.pio/qemu}"
IMG="$OUT/c3flash.bin"
B="$PROJ/.pio/build/c3qemu"
Q=$(ls ~/.espressif/tools/qemu-riscv32/*/qemu/bin/qemu-system-riscv32 | tail -1)
ESPTOOL=~/.espressif/python_env/idf5.5_py3.12_env/bin/esptool.py
mkdir -p "$OUT"

case "${1:-run}" in
build)
    (cd "$PROJ" && ~/.pio-venv/bin/pio run -e c3qemu)
    "$ESPTOOL" --chip esp32c3 merge_bin -o "$IMG" --fill-flash-size 4MB \
        0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0x20000 "$B/firmware.bin"
    ;;
update)
    (cd "$PROJ" && ~/.pio-venv/bin/pio run -e c3qemu)
    python3 - "$IMG" "$B" <<'PY'
import sys
img = bytearray(open(sys.argv[1], 'rb').read())
for off, f in ((0x0, 'bootloader.bin'), (0x8000, 'partitions.bin'), (0x20000, 'firmware.bin')):
    d = open(sys.argv[2] + '/' + f, 'rb').read()
    img[off:off + len(d)] = d
img[0xf000:0x11000] = b'\xff' * 0x2000      # otadata: boot ota_0
open(sys.argv[1], 'wb').write(img)
PY
    ;;
run|console)
    [ -f "$IMG" ] || { echo "no image: run '$0 build' first"; exit 1; }
    if [ "$1" = console ]; then
        SER="-chardev socket,id=c0,host=127.0.0.1,port=5555,server=on,wait=off -serial chardev:c0"
    else
        SER="-serial file:$OUT/qemu-c3.log"
    fi
    timeout "${2:-600}" "$Q" -nographic -M esp32c3 \
        -drive file="$IMG",if=mtd,format=raw $SER -monitor none \
        -nic user,model=open_eth,hostfwd=tcp::18080-:80 || true
    ;;
*)  echo "usage: $0 build|update|run [s]|console [s]"; exit 1 ;;
esac
