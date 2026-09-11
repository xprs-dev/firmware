#!/bin/bash
# Build the images the catalogue publishes, the way a stranger should get
# them: no network name, no password, no owner.
#
#   tools/build-published.sh tdeck esp32c3-mini ...     (default: all of them)
#
# A board's src/wifi_secrets.h and src/fw_secrets.h are gitignored, but what
# is in them is compiled into the image, and the prebuilt images once shipped
# the bench's WiFi. So for each board this swaps in the empty
# wifi_secrets.h.example, blanks FW_DEFAULT_OWNER (the firmware-signing key
# FW_DEFAULT_KEY stays: that is how a published image takes signed updates),
# builds clean, refuses the image if it still carries the bench SSID, its
# password or the owner's npub, and copies it into models/<board>/prebuilt
# with tools/scripts/collect_prebuilt.py. The bench files are put back
# however it ends.
#
# An image built this way comes up unowned, asks to be claimed (XPRS.md
# 11.9) and is set up from the phone (11.10).
#
# Builds run one at a time under ~/bin/android-build-locked when it exists:
# two ESP-IDF builds at once freeze a 16 GB machine.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
ALL="tdeck tdongle-s3 m5stack-core heltec-v3 epaper-1in54 esp32c3-mini generic"
BOARDS="${*:-$ALL}"
PIO="${PIO:-$HOME/.pio-venv/bin/pio}"
LOCK="$HOME/bin/android-build-locked"
[ -x "$LOCK" ] || LOCK=""
BACKUP=$(mktemp -d)

restore() {
    for b in $BOARDS; do
        src="$ROOT/models/$b/firmware/src"
        for f in wifi_secrets.h fw_secrets.h; do
            if [ -f "$BACKUP/$b.$f" ]; then cp -p "$BACKUP/$b.$f" "$src/$f"; fi
        done
    done
    rm -rf "$BACKUP"
    echo "bench secrets restored"
}
trap restore EXIT

# What must not be in a published image: every board's bench SSID, password
# and owner npub, from the files about to be swapped out.
declare -a NEEDLES=()
for b in $BOARDS; do
    src="$ROOT/models/$b/firmware/src"
    [ -d "$src" ] || { echo "$b: no models/$b/firmware/src"; exit 1; }
    for f in wifi_secrets.h fw_secrets.h; do
        [ -f "$src/$f" ] && cp -p "$src/$f" "$BACKUP/$b.$f"
    done
    for f in wifi_secrets.h fw_secrets.h; do
        [ -f "$src/$f" ] || continue
        while IFS= read -r v; do
            [ ${#v} -ge 4 ] && NEEDLES+=("$v")
        done < <(sed -nE 's/^#define[[:space:]]+(WIFI_SSID|WIFI_PASS|FW_DEFAULT_OWNER)[[:space:]]+"([^"]*)".*/\2/p' "$src/$f")
    done
done

fail=0
for b in $BOARDS; do
    fw="$ROOT/models/$b/firmware"
    src="$fw/src"
    cp "$src/wifi_secrets.h.example" "$src/wifi_secrets.h"
    if [ -f "$BACKUP/$b.fw_secrets.h" ]; then
        sed -E 's/^(#define[[:space:]]+FW_DEFAULT_OWNER[[:space:]]+)"[^"]*"/\1""/' \
            "$BACKUP/$b.fw_secrets.h" > "$src/fw_secrets.h"
    else
        cp "$src/fw_secrets.h.example" "$src/fw_secrets.h"
    fi
    if grep -q XAUTH_BENCH_NPUB "$src"/*.c "$src"/*.h "$fw/platformio.ini" 2>/dev/null; then
        echo "$b: XAUTH_BENCH_NPUB is defined; a published image must not carry a bench owner"
        fail=1; break
    fi
    env=$(sed -nE 's/^\[env:([A-Za-z0-9_]+)\].*/\1/p' "$fw/platformio.ini" | head -1)
    touch "$src/main.c"
    # And the project: a component whose CMakeLists.txt gained a source is
    # not reconfigured otherwise, and the link then fails on the new symbols.
    touch "$fw/CMakeLists.txt"
    out=$( (cd "$fw" && $LOCK "$PIO" run -e "$env" 2>&1) | grep -E "SUCCESS|FAILED|error:" | tail -1)
    bin="$fw/.pio/build/$env/firmware.bin"
    hits=0
    for n in "${NEEDLES[@]}"; do
        c=$(grep -c -a -F -- "$n" "$bin" 2>/dev/null || true)
        hits=$((hits + ${c:-0}))
    done
    echo "$b ($env): $out | bench strings in the image: $hits"
    if [ "$hits" != 0 ] || ! echo "$out" | grep -q SUCCESS; then fail=1; break; fi
    python3 tools/scripts/collect_prebuilt.py "$b" | tail -1
done
exit $fail
