#!/bin/bash
# Ask a station something over the air, from the bench.
#
#   tools/xprs_cmd.sh --gateway 192.168.178.102 --to X3R8XX --cmd zdiag \
#       --from X38364 --owner-nsec ~/.xprs/owner.nsec [key=value ...]
#
# The gateway is any station on the LAN with a radio -- the bench dongle, a
# deck -- reached over HTTP. The command is signed here with the owner's key
# (tool/sign_command.dart in xprs-flutter), handed to the gateway's
# /api/xprs/send, which airs it on every bearer it has, and the results come
# back the same way: the gateway hears them and archives them, and this
# script polls its history for rows that carry the command's id.
#
# Commands (private z words until agreed -- docs/device.md):
#   zdiag                 one frame: fw uptime peers zr zm zh zn zs zp [zc]
#   zcore                 crash task + backtrace PCs; tools/xprs_bt.sh resolves
#   zlog [since=.. until=.. zq=word zl=last]
#                         the log, newest first, paged: 202, 206 per line, 200/206
#
# Exit 0 on a terminal code (200/4xx/5xx), 1 on timeout.
set -u
GW= TO= CMD= FROM= NSEC= TIMEOUT=180 FLUTTER=${XPRS_FLUTTER:-$HOME/code/xprs/xprs-flutter}
EXTRA=()
while [ $# -gt 0 ]; do
  case $1 in
    --gateway) GW=$2; shift 2;;    --to) TO=$2; shift 2;;
    --cmd) CMD=$2; shift 2;;       --from) FROM=$2; shift 2;;
    --owner-nsec) NSEC=$2; shift 2;; --timeout) TIMEOUT=$2; shift 2;;
    --flutter) FLUTTER=$2; shift 2;;
    *=*) EXTRA+=("$1"); shift;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
[ -n "$GW" ] && [ -n "$TO" ] && [ -n "$CMD" ] && [ -n "$FROM" ] && [ -n "$NSEC" ] || {
  sed -n 2,20p "$0"; exit 2; }

# One ask, one collection. A result is not retried by the station and not
# relayed (25.3), so a single lost frame is silence -- and the protocol's
# answer to silence is to ask again, which is free because the id makes a
# repeat idempotent (25.4). So: ask, wait, and ask once more before giving up.
ask_once() {
WIRE=$(cd "$FLUTTER" && dart run tool/sign_command.dart --to "$TO" --cmd "$CMD" \
         --from "$FROM" --nsec-file "$(realpath "$NSEC")" "${EXTRA[@]}") || {
  echo "signing failed" >&2; exit 2; }
echo "ask : $WIRE"

REPLY=$(curl -fsS --max-time 10 -X POST "http://$GW/api/xprs/send" \
          -H 'Content-Type: text/plain' --data-binary "$WIRE") || {
  echo "gateway refused: $REPLY" >&2; exit 2; }
ID=$(echo "$REPLY" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
[ -n "$ID" ] || { echo "no id in $REPLY" >&2; exit 2; }
echo "id  : $ID  (waiting up to ${TIMEOUT}s for $TO)"

# Results are archived by the gateway like anything it hears. The app
# boards filter with only=/call=, the dongle with type=/from=; try both.
seen=""
done_code=0
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  sleep 2
  rows=$( { curl -fsS --max-time 5 "http://$GW/api/xprs/history?only=result&call=$TO&limit=40" 2>/dev/null;
            curl -fsS --max-time 5 "http://$GW/api/xprs?type=result&from=$TO&limit=40&recent=1" 2>/dev/null; } \
          | grep -oE '"wire":"[^"]*"' | sed 's/^"wire":"//; s/"$//' | grep " r:$ID " )
  [ -z "$rows" ] && continue
  while IFS= read -r w; do
    key=$(echo "$w" | md5sum | cut -c1-8)
    case " $seen " in *" $key "*) continue;; esac
    seen="$seen $key"
    code=$(echo "$w" | sed -n 's/.* code:\([0-9]*\).*/\1/p')
    # sig: sits before m: (the signer splices it there), so strip it anywhere
    body=$(echo "$w" | sed 's/^t:result f:[^ ]* d:[^ ]* \(ts:[^ ]* \)\?r:[0-9a-f]* //; s/ sig:[^ ]*//')
    echo "$code : $body"
    case $code in 200|4??|5??) done_code=1;; esac
  done <<< "$rows"
  # Finish the batch before leaving: a duplicate ask (a gateway airs on
  # every bearer) produces a second answer, and the terminal one is not
  # always the first row polled.
  [ "${done_code:-0}" = 1 ] && exit 0
done
return 1
}

ask_once && exit 0
echo "no answer in ${TIMEOUT}s -- asking once more (a lost result is not retried by the station)" >&2
ask_once && exit 0
echo "timeout: no terminal answer from $TO" >&2
exit 1
