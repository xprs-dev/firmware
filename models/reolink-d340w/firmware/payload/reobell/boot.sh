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

# The device key does NOT live beside the binary: in the baked build the rootfs
# is mounted read-only, so a key written there cannot be created at all and the
# daemon comes up with nothing to sign with (silence on the air, no still
# server, and no way to tell from outside; that is exactly what firmVer 4665
# did). /mnt/para is the camera's writable config partition, which is what a
# key needs, and it survives both a reboot and a firmware flash: measured
# across the 4668 -> 4669 update, the doorbell came back as the same callsign.
# Not *.key, and not *.crt: the camera's own `device` binary keeps its TLS
# material in /mnt/para and clears it with `rm /mnt/para/*.crt` and
# `rm /mnt/para/*.key`. A device key named reobell.key is deleted by that glob
# between boots, which is the whole reason this doorbell used to come back
# under a new callsign after every restart -- five identities in one hour on
# 2026-09-23 -- and why the file is called reobell.nsec now.
KEYFILE="${KEYFILE:-/mnt/para/reobell.nsec}"
KEYDIR=$(dirname "$KEYFILE")

# /mnt/para is a UBIFS volume that S00_PreReady mounts (ubiattach -m 8), and
# until that has happened the same path is a bare directory in the read-only
# rootfs. Writing the key there would go nowhere, which is why this waits for
# the mount instead of judging it once: a key that lands on the wrong side of
# that race gives the doorbell a new callsign on every boot, and every station
# on the LAN meets a stranger. The probe runs in a subshell because a failed
# redirection on a special builtin ends the whole script in dash.
i=0
while [ "$i" -lt 24 ]; do
  mkdir -p "$KEYDIR" 2>/dev/null
  if ( : > "$KEYDIR/.reobell_probe" ) 2>/dev/null; then
    rm -f "$KEYDIR/.reobell_probe"
    break
  fi
  i=$((i + 1))
  echo "$(date -u) waiting for $KEYDIR to be writable ($i)"
  sleep 5
done
if [ "$i" -ge 24 ]; then
  # Announcing under a fresh callsign every boot is wrong, but it is visible
  # on the air, where a daemon that cannot start is not.
  echo "$(date -u) $KEYDIR never became writable; using /mnt/tmp, so this"
  echo "$(date -u) camera takes a new callsign on every boot. Fix KEYFILE."
  KEYFILE=/mnt/tmp/reobell.nsec
fi
export REOBELL_KEY="$KEYFILE"

# An identity from before the rename, if the camera has not eaten it yet.
if [ ! -s "$KEYFILE" ] && [ -s "$KEYDIR/reobell.key" ]; then
  cp "$KEYDIR/reobell.key" "$KEYFILE" && echo "$(date -u) kept the old callsign"
fi

# Generate the device keypair once (an X4 callsign derived from it). The
# private key stays where the admin password already is.
[ -s "$REOBELL_KEY" ] || "$DIR/reobell" keygen
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
