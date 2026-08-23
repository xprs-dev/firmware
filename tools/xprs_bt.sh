#!/bin/bash
# Resolve the PCs a station aired in its cmd:zcore answer against the ELF
# that push_firmware.sh kept for that version:
#
#   tools/xprs_bt.sh tdeck 0.3.3 4200a1f2 40378e4d ...
#
# The ELF is ~/.xprs/elf/<board>-<version>.elf. Without the matching ELF the
# numbers mean nothing -- which is why the push keeps one.
set -u
BOARD=${1:?usage: xprs_bt.sh <board> <version> <pc...>}
VER=${2:?usage: xprs_bt.sh <board> <version> <pc...>}
shift 2
ELF=$HOME/.xprs/elf/$BOARD-$VER.elf
[ -f "$ELF" ] || { echo "no $ELF -- the push that installed $VER did not keep its ELF" >&2; exit 2; }
case $BOARD in
  m5stack-core) A2=$(ls ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line 2>/dev/null | head -1);;
  *)            A2=$(ls ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line 2>/dev/null | head -1);;
esac
[ -n "$A2" ] || { echo "no addr2line in ~/.platformio/packages/toolchain-xtensa-esp-elf" >&2; exit 2; }
for pc in "$@"; do
  pc=${pc#0x}
  "$A2" -pfiaC -e "$ELF" "0x$pc"
done
