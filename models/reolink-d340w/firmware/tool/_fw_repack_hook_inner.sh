#!/bin/bash
# Runs INSIDE a root debian container. Rebuilds ONLY the rootfs section of the
# D340W .pak with the start_app SD-boot hook injected, using the proven params
# (mkfs.ubifs -m 2048 -e 126976 -c 239, lzo; ubinize -m 2048 -p 131072). The
# app section is left untouched. Verifies the result.
set -euo pipefail
PAK_IN="$1"; HOOK="$2"; OUT="$3"
W=/tmp/fwwork; rm -rf "$W"; mkdir -p "$W"; cd "$W"

echo "== install tools =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq mtd-utils liblzo2-2 file python3-pip >/dev/null
pip install -q --break-system-packages pakler ubi_reader >/dev/null 2>&1 || pip install -q pakler ubi_reader >/dev/null

echo "== split sections =="
python3 -m pakler -e -d sec "$PAK_IN"
ls sec

ROOTBIN=$(ls sec/*rootfs*.bin)
SECNUM=$(basename "$ROOTBIN" | sed -E 's/^0*([0-9]+)_.*/\1/')
echo "rootfs section=$SECNUM file=$ROOTBIN"

echo "== extract rootfs UBIFS (as root, preserve attrs) =="
ubireader_extract_files -k -o tree "$ROOTBIN" 2>&1 | tail -1 || true
TREE=$(find tree -maxdepth 3 -type d -name rootfs | head -1)
[ -n "$TREE" ] || { echo "no rootfs tree"; exit 1; }
echo "tree: $TREE  files=$(find "$TREE" -type f | wc -l)"

echo "== inject hook into start_app =="
SA="$TREE/etc/init.d/start_app"
[ -f "$SA" ] || { echo "start_app missing"; exit 1; }
grep -q 'reobell:' "$SA" && { echo "hook already present"; } || cat "$HOOK" >> "$SA"
tail -6 "$SA"

echo "== rebuild rootfs UBIFS + UBI (proven params) =="
mkfs.ubifs -r "$TREE/" -o rootfs.ubifs -m 2048 -e 126976 -c 239
UBIFS_SZ=$(stat -c%s rootfs.ubifs)
LEBCNT=$(( (UBIFS_SZ + 126976 - 1) / 126976 )); VOLSZ=$(( LEBCNT * 126976 ))
cat > rootfs.cfg <<CFG
[rootfs]
mode=ubi
image=rootfs.ubifs
vol_id=0
vol_name=rootfs
vol_size=$VOLSZ
vol_type=dynamic
vol_alignment=1
CFG
ubinize -o rootfs.ubi -m 2048 -p 131072 rootfs.cfg
echo "new rootfs.ubi size: $(stat -c%s rootfs.ubi)  (partition cap 33554432)"
[ "$(stat -c%s rootfs.ubi)" -le 33554432 ] || { echo "TOO BIG for partition"; exit 1; }

echo "== repack pak (replace rootfs section only) =="
cp "$PAK_IN" out.pak
python3 -m pakler out.pak -r -n "$SECNUM" -f rootfs.ubi -o "$OUT"
echo "== verify patched pak =="
python3 -m pakler -l "$OUT" | tail -3

echo "== round-trip: re-extract patched rootfs, confirm hook present =="
python3 -m pakler -e -d sec2 "$OUT"
ubireader_extract_files -k -o tree2 sec2/*rootfs*.bin 2>&1 | tail -1 || true
SA2=$(find tree2 -path '*etc/init.d/start_app' | head -1)
if grep -q 'reobell:' "$SA2"; then echo "OK: hook present in patched image"; else echo "FAIL: hook missing"; exit 1; fi
# diff the trees (should differ only by start_app)
TREE2=$(dirname "$(dirname "$(dirname "$SA2")")")
echo "changed files vs original:"; diff -rq "$TREE" "$TREE2" 2>/dev/null | grep -v start_app | head || true
echo "DONE."
