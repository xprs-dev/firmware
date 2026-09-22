#!/bin/sh
# Entry point run by the firmware boot hook. Supervises the camera-side XPRS
# daemon: a one-time device identity, a periodic signed presence beacon, and the
# ring poller. All of it is editable here on the SD card, no reflash.
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/config"
chmod +x "$DIR/reobell" "$DIR/xprsbell.sh" 2>/dev/null

# The signer reads its key and presence fields from the environment.
export REOBELL_KEY="$DIR/reobell.key"
export REOBELL_NICK="${NICK:-frontdoor}"
export REOBELL_URL="$URL"

# Generate the device keypair once (an X4 callsign derived from it). The private
# key stays on the SD card, like the admin password already does.
[ -f "$REOBELL_KEY" ] || "$DIR/reobell" keygen
CALL=$("$DIR/reobell" callsign 2>/dev/null | sed -n 's/callsign=//p')
echo "$(date -u) reobell boot callsign=$CALL"

# Signed presence announcement every 5 minutes, so other XPRS stations learn
# this device (its callsign, key and nick) even between rings.
(
  while :; do
    "$DIR/reobell" identity $BCAST >/dev/null 2>&1
    sleep 300
  done
) &

# Ring poller: restarts if it ever exits.
while :; do
  "$DIR/xprsbell.sh"
  sleep 5
done
