#!/bin/sh
# Host-side test for the `hears:` ladder (XPRS.md 10.6.3, 10.6.4).
#
# The ranking, the signal bucket and the three tiers are arithmetic over a
# sixteen-row array. Checking them by flashing two boards and reading serial
# would take a build per case and could never produce the sixteenth neighbour
# at all -- there is only one other board in the room.
set -e
cd "$(dirname "$0")"

XPRS=../geogram_xprs
SHA=../geogram_xprsindex/test_sha256_host.c   # the codec's sha256 on the host

gcc -Wall -Wextra -Werror -O1 -DXST_HOST_TEST \
    -I. -I"$XPRS" \
    -o /tmp/test_xprs_station \
    xprs_station.c "$XPRS"/xprs.c test_xprs_station_host.c "$SHA" -lm

/tmp/test_xprs_station
