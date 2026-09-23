#!/bin/sh
# Entry point run by the firmware boot hook (or by start_app in the baked
# build). One process does the lot now: it holds a single login against the
# camera's own api.cgi, watches the door, airs what it sees as signed XPRS
# observations, and serves the still those observations point at.
#
# The shell poller this replaced logged in again on every token error and
# leaked a lease on every restart, and could only say `t:message`. Everything
# it did is inside the reobell binary; what is left here is a supervisor.
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/config"
chmod +x "$DIR/reobell" 2>/dev/null

# The daemon reads everything from the environment.
export REOBELL_KEY="$DIR/reobell.key"
export REOBELL_NICK="${NICK:-frontdoor}"
export REOBELL_USER="${USER:-admin}"
export REOBELL_PASS="$PASSWORD"
export REOBELL_API="${API:-http://127.0.0.1}"
export REOBELL_HTTP_PORT="${HTTP_PORT:-8080}"
# Fixed address for the picture, when the camera's own is not what a phone
# should be told (a port forward, say). Empty means "work it out at run time",
# which is what a DHCP lease needs.
export REOBELL_URL="$URL"
[ -n "$POLL" ] && export REOBELL_POLL_MS=$((POLL * 1000))
[ -n "$MOTION_DEBOUNCE" ] && export REOBELL_MOTION_DEBOUNCE_S="$MOTION_DEBOUNCE"

# Generate the device keypair once (an X4 callsign derived from it). The
# private key stays where the admin password already is.
[ -f "$REOBELL_KEY" ] || "$DIR/reobell" keygen
CALL=$("$DIR/reobell" callsign 2>/dev/null | sed -n 's/callsign=//p')
echo "$(date -u) reobell boot callsign=$CALL"

# Supervise it: a doorbell that dies at three in the morning is a doorbell
# that rings again at three in the morning. The pause is there so a daemon
# that cannot start does not spin against the camera's session count.
while :; do
  "$DIR/reobell" run $BCAST
  echo "$(date -u) reobell exited, restarting in 10s"
  sleep 10
done
