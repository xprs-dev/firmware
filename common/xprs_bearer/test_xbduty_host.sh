#!/bin/sh
# Host-side test for the duty ledger and the priority queue: a bare xb_t with
# a fake clock and a fake radio, no bearer module at all -- the accounting is
# generic and this is the proof.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -DXB_HOST_TEST -I. -I"$XPRS" \
    -o /tmp/test_xbduty \
    xprsbearer.c xb_airtime.c "$XPRS"/xprs.c test_xbduty_host.c "$SHA"
/tmp/test_xbduty
