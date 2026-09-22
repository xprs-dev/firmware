#!/usr/bin/env bash
# Decompress a Reolink D340W .pak: split sections, then extract the rootfs and
# app filesystems. Reads a local file and never touches a camera. Output goes to
# tool/work/unpacked/, which is gitignored. What is in that tree, and what
# was read out of it, is in ../../docs/firmware-analysis.md.
set -euo pipefail

FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # models/reolink-d340w/firmware
PAK="${1:-$(ls "$FW"/stock/DB_566128M5MP_W.*.pak | head -1)}"   # stock/fetch-stock.sh puts it there
OUT="$FW/tool/work/unpacked"
SEC="$OUT/sections"

echo "pak: $PAK"
mkdir -p "$SEC"

echo "== split sections =="
python3 -m pakler -e -d "$SEC" "$PAK"
ls -la "$SEC"

# rootfs/app are UBI images wrapping a UBIFS volume -> use ubi_reader.
extract_fs() {
  local sect="$1" dest="$2"
  [ -s "$sect" ] || { echo "  (no $sect)"; return; }
  echo "== $sect: magic=$(xxd -p -l4 "$sect") =="
  command -v ubireader_extract_files >/dev/null 2>&1 || {
    echo "  ubi_reader missing: pip install ubi_reader"; return; }
  rm -rf "$dest"; mkdir -p "$dest"
  ubireader_extract_files -k -o "$dest" "$sect" 2>&1 | grep -v 'Operation not permitted' | tail -1 || true
  echo "  -> extracted $(find "$dest" -type f 2>/dev/null | wc -l) files to $dest"
}

# section files are named by pakler: look for rootfs / app
ROOTFS=$(ls "$SEC"/*rootfs* 2>/dev/null | head -1 || true)
APP=$(ls "$SEC"/*app* 2>/dev/null | head -1 || true)
[ -n "$ROOTFS" ] && extract_fs "$ROOTFS" "$OUT/rootfs"
[ -n "$APP" ]    && extract_fs "$APP"    "$OUT/app"

echo "== done. tree: $OUT =="
