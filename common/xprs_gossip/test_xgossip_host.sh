#!/bin/sh
# Host-side test for the gossip store: builds xgossip.c over a temp directory,
# so the three walls of 36.9.4 -- signer-credited, per-signer quota, byte
# budget -- and the L2/L3 ranking are all checked without a board or a card.
set -e
cd "$(dirname "$0")"

gcc -Wall -Wextra -Werror -O1 -DXGOSSIP_HOST_TEST \
    -I. -o /tmp/test_xgossip \
    xgossip.c test_xgossip_host.c

/tmp/test_xgossip
