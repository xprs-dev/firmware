#!/usr/bin/env bash
# Produce a D340W .pak with the SD-boot HOOK variant (firmVer 4663: one line
# appended to start_app, everything else on the SD card), using mtd-utils inside
# Docker so the host needs no root. The BAKED variant, which is what the unit
# runs, is build_reobell_pak.sh. Output: tool/work/<name>_reobell.pak
set -euo pipefail
FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # models/reolink-d340w/firmware
PAK="${1:-$(ls "$FW"/stock/DB_566128M5MP_W.*.pak | head -1)}"
HOOK="$FW/firmware-hook.sh"
OUTDIR="$FW/tool/work"; mkdir -p "$OUTDIR"
OUT="$OUTDIR/$(basename "${PAK%.pak}")_reobell.pak"
echo "pak:  $PAK"
echo "out:  $OUT"
docker run --rm -v "$FW:/fw" -w /fw debian:12 \
  bash tool/_fw_repack_hook_inner.sh \
  "/fw/${PAK#$FW/}" "/fw/${HOOK#$FW/}" "/fw/${OUT#$FW/}"
echo; echo "patched pak: $OUT"
ls -la "$OUT" 2>/dev/null
