#!/bin/sh
# Round-trip: pack a bundle with the tool, parse it with the firmware code.
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

[ -x ../../../tools/wrenchc ] || ../../../tools/build_wrenchc.sh >/dev/null

cat > "$OUT/one.w" <<'W'
function boot() { return 1; }
W
cat > "$OUT/two.w" <<'W'
function tick(ms) { return ms; }
W

# Module "two" asks for a 10 ms tick on purpose: the parser must clamp it.
../../../tools/mkbundle.py build \
    --board tdeck --id spike --version 0.1.0 \
    --types t:message,t:report \
    --out "$OUT/spike.xscb" \
    "one=$OUT/one.w" "two=$OUT/two.w:10" > "$OUT/build.txt"

SHA=$(sed -n 's/.*xprsscr1 tdeck spike 0.1.0 [0-9]* \([0-9a-f]*\).*/\1/p' "$OUT/build.txt")
[ -n "$SHA" ] || { echo "FAIL: could not read the sha out of the packer output"; exit 1; }

gcc -std=gnu99 -Wall -Wextra -Werror -O1 -DXPRSSIG_HOST_TEST \
    -I.. -I../../xprs_sig -o "$OUT/t" \
    test_bundle_host.c ../xs_bundle.c ../../xprs_sig/xprssig.c -lcrypto
"$OUT/t" "$OUT/spike.xscb" "$SHA"

# --- and now the half that decides whether foreign code runs -------------
gcc -std=gnu99 -Wall -Wextra -Werror -O1 -DXPRSSIG_HOST_TEST \
    -I.. -I../../xprs_sig -o "$OUT/v" \
    test_verify_host.c ../xs_bundle.c ../../xprs_sig/xprssig.c -lcrypto
"$OUT/v" "$OUT/spike.xscb"
