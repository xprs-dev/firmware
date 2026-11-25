#!/bin/sh
# Host test for xb_qos (QoS.md) and the bearer under it.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -DXB_HOST_TEST -I. -I"$XPRS" \
    -o /tmp/test_xbqos \
    xprsbearer.c xb_airtime.c xb_qos.c "$XPRS"/xprs.c test_xbqos_host.c "$SHA"
/tmp/test_xbqos
