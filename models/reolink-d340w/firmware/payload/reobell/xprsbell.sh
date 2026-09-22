#!/bin/sh
# Camera-side doorbell -> XPRS. Polls the local api.cgi for the visitor button
# and, on the rising edge, has `reobell` sign and broadcast the XPRS wire.
# POSIX/busybox sh. The signing key lives in $REOBELL_KEY (set by boot.sh).
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/config"
export REOBELL_KEY="${REOBELL_KEY:-$DIR/reobell.key}"
API="http://127.0.0.1/cgi-bin/api.cgi"
TOKEN=""

login() {
  r=$(wget -q -O - --post-data="[{\"cmd\":\"Login\",\"action\":0,\"param\":{\"User\":{\"Version\":\"0\",\"userName\":\"$USER\",\"password\":\"$PASSWORD\"}}}]" "$API?cmd=Login" 2>/dev/null | tr -d ' \n\r\t')
  TOKEN=$(echo "$r" | sed -n 's/.*"Token":{"leaseTime":[0-9]*,"name":"\([^"]*\)".*/\1/p' | head -1)
  [ -n "$TOKEN" ]
}
ts() { date -u +%Y-%m-%d_%H:%M:%S; }

prev=0
while :; do
  [ -n "$TOKEN" ] || login || { sleep 5; continue; }
  ev=$(wget -q -O - --post-data="[{\"cmd\":\"GetEvents\",\"action\":0,\"param\":{\"channel\":0}}]" "$API?cmd=GetEvents&token=$TOKEN" 2>/dev/null | tr -d ' \n\r\t')
  case "$ev" in
    *'"rspCode"'*'-6'*) TOKEN=""; continue ;;   # token expired -> re-login
  esac
  cur=$(echo "$ev" | sed -n 's/.*"visitor":{"alarm_state":\([0-9]\).*/\1/p' | head -1)
  [ -n "$cur" ] || cur=0
  if [ "$prev" = "0" ] && [ "$cur" = "1" ]; then
    # f: is a placeholder; reobell forces it to the callsign derived from the
    # key, then signs (sig: before m:) and broadcasts to :4242.
    wire="t:message f:X4DOOR ts:$(ts) scope:local m:$MESSAGE"
    "$DIR/reobell" sign-send "$wire" $BCAST >/dev/null 2>&1 && echo "$(ts) RING signed+sent"
  fi
  prev=$cur
  sleep "${POLL:-1}"
done
