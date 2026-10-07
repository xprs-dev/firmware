#!/bin/sh
# Host-side test for the flash mail store (xmb.c), on a RAM flash with the
# nRF52's write rules. Platform-free.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -I. -I"$XPRS" -o /tmp/test_xmb xmb.c test_xmb_host.c "$XPRS"/xprs.c "$SHA"
/tmp/test_xmb
