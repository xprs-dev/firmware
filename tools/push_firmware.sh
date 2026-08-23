#!/usr/bin/env bash
# Push a signed firmware to a station over the network -- no cable.
#
#   tools/push_firmware.sh --host 192.168.178.140 --board tdeck \
#       --version 0.3.0 --bin models/tdeck/firmware/.pio/build/tdeck/firmware.bin \
#       --fw-nsec ~/.xprs/fw.nsec --owner-nsec ~/.xprs/owner.nsec --from X1Q3Q5
#
# Two signatures, two keys, and they are different on purpose (XPRS.md 25.8):
#   the APPROVAL  -- sign_firmware.dart, the publisher key the station pins as
#                    `fwkey`; signs "xprsfw1 <board> <version> <size> <sha256>"
#   the AUTH      -- sign_command.dart, an owner key from the station's
#                    own1..own4; a signed t:command bound to THIS approval by
#                    zsha = sha256(approval)[0:16], so the header cannot be
#                    replayed to install a different image.
# Both tools live in the flutter checkout (they share its crypto); point
# XPRS_FLUTTER at it (default ../xprs-flutter).
#
# The version MUST equal the string the image embeds (esp_app_desc.version:
# version.txt, or `git describe` when there is none). A mismatch is not a
# refusal -- the new image boots, concludes it was rolled back, and says so.
# The script reads it out of the binary and refuses to continue if they differ.
set -euo pipefail
HOST= BOARD= VERSION= BIN= FWNSEC= OWNNSEC= FROM= ELF=
FLUTTER=${XPRS_FLUTTER:-"$(dirname "$0")/../../xprs-flutter"}
while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST=$2; shift 2;;        --board) BOARD=$2; shift 2;;
    --version) VERSION=$2; shift 2;;  --bin) BIN=$2; shift 2;;
    --fw-nsec) FWNSEC=$2; shift 2;;   --owner-nsec) OWNNSEC=$2; shift 2;;
    --from) FROM=$2; shift 2;;        --flutter) FLUTTER=$2; shift 2;;
    --elf) ELF=$2; shift 2;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
for v in HOST BOARD VERSION BIN FWNSEC OWNNSEC FROM; do
  [ -n "${!v}" ] || { echo "missing --$(echo $v | tr A-Z a-z | sed 's/fwnsec/fw-nsec/;s/ownnsec/owner-nsec/')" >&2; exit 2; }
done
[ -f "$BIN" ] || { echo "no such image: $BIN" >&2; exit 2; }
[ -d "$FLUTTER/tool" ] || { echo "flutter checkout not found at $FLUTTER (set XPRS_FLUTTER)" >&2; exit 2; }

# The station's callsign and what it is running, before anything is signed.
# /api/diag, not /api/status: every board with the updater has diag (it is
# part of the same contract), and it carries both the callsign and the
# running version. /api/status is the app boards' extra.
DIAG=$(curl -fsS --max-time 10 "http://$HOST/api/diag") || { echo "station at $HOST does not answer /api/diag" >&2; exit 1; }
CALL=$(printf '%s' "$DIAG" | sed -n 's/.*"callsign":"\([A-Z0-9]*\)".*/\1/p')
RUNNING=$(printf '%s' "$DIAG" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p')
# Older app-board images carry the callsign only in /api/status.
[ -n "$CALL" ] || CALL=$(curl -fsS --max-time 10 "http://$HOST/api/status" 2>/dev/null | sed -n 's/.*"callsign":"\([A-Z0-9]*\)".*/\1/p')
[ -n "$CALL" ] || { echo "no callsign in /api/diag or /api/status -- is this an XPRS station?" >&2; exit 1; }
echo "station $CALL at $HOST, running ${RUNNING:-?}"

# The version embedded in the image, so the approval names the truth.
EMBEDDED=$(strings -n 4 "$BIN" | grep -m1 -oE "^$VERSION$" || true)
if [ -z "$EMBEDDED" ]; then
  echo "the image does not embed version '$VERSION' -- refusing (embedded: $(strings -n 6 "$BIN" | grep -m1 -E '^[0-9a-f]{7}(-dirty)?$|^[0-9]+\.[0-9]+\.[0-9]+' || echo '?'))" >&2
  exit 1
fi

SIZE=$(stat -c%s "$BIN")
echo "approving $BOARD $VERSION ($SIZE bytes)"
# sign_firmware.dart prints a report ("signature   : <60 base85>") and then
# a manifest; the approval is that one line.
SIG=$(cd "$FLUTTER" && dart run tool/sign_firmware.dart --board "$BOARD" --version "$VERSION" --bin "$(realpath "$BIN")" --nsec-file "$(realpath "$FWNSEC")" \
      | sed -n 's/^signature *: *//p' | head -1)
[ ${#SIG} -eq 60 ] || { echo "approval is not 60 base85 characters: '$SIG'" >&2; exit 1; }
ZSHA=$(printf '%s' "$SIG" | sha256sum | cut -c1-16)
AUTH=$(cd "$FLUTTER" && dart run tool/sign_command.dart --to "$CALL" --cmd update --from "$FROM" --nsec-file "$(realpath "$OWNNSEC")" "ver=$VERSION" "zsha=$ZSHA")

echo "pushing ..."
# A small board can be too busy to check the signature while the image it
# is checking is still arriving (503), and a starved socket simply dies
# (curl 52, empty reply). Both are "try again", not "no", so try -- three
# times, spaced, before believing it.
for attempt in 1 2 3; do
  HTTP=$(curl -sS --max-time 300 -o /tmp/push_reply.json -w '%{http_code}' \
    -X POST "http://$HOST/api/update" \
    -H "X-XPRS-Fw-Version: $VERSION" -H "X-XPRS-Fw-Sig: $SIG" -H "X-XPRS-Auth: $AUTH" \
    -H "Content-Type: application/octet-stream" --data-binary "@$BIN") || true
  echo "reply $HTTP: $(cat /tmp/push_reply.json 2>/dev/null)"
  case "$HTTP" in
    200) break;;
    503|000) [ "$attempt" = 3 ] && exit 1
             echo "  the station was too busy to take it; waiting 15 s and pushing again"
             sleep 15;;
    *) exit 1;;
  esac
done
[ "$HTTP" = 200 ] || exit 1

# The station restarts into the new image and must prove itself within
# two minutes, or the old one comes back. Watch it do so.
# Keep the ELF that matches what is now on the roof. A coredump is only
# readable against the exact build that wrote it (espcoredump checks the app
# SHA), and a station that hangs a week from now cannot be rebuilt bit-for-bit.
[ -z "$ELF" ] && [ -f "$(dirname "$BIN")/firmware.elf" ] && ELF="$(dirname "$BIN")/firmware.elf"
if [ -n "$ELF" ] && [ -f "$ELF" ]; then
  mkdir -p "$HOME/.xprs/elf"
  cp "$ELF" "$HOME/.xprs/elf/$BOARD-$VERSION.elf"
  echo "kept $HOME/.xprs/elf/$BOARD-$VERSION.elf for coredumps"
fi
echo "installed; waiting for the station to come back on $VERSION ..."
for i in $(seq 1 40); do
  sleep 5
  V=$(curl -fsS --max-time 5 "http://$HOST/api/diag" 2>/dev/null | sed -n 's/.*"version":"\([^"]*\)".*/\1/p') || true
  [ -n "$V" ] && { echo "  up: running $V ($((i*5)) s)"; [ "$V" = "$VERSION" ] && break; }
done
echo "self-test verdict lands at 120 s of uptime; then:"
echo "  curl http://$HOST/api/diag     -> \"part\":{\"state\":...}  (VALID = kept, else it rolled back)"
