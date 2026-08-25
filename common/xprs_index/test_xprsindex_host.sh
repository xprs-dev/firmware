#!/bin/sh
# Host-side test for the XPRS index: builds xprsindex.c against the real XPRS
# codec and runs it over a temp directory, so the record layout, the derived
# indexes and the section 36 serving rule are all checked without a card.
#
# It writes 18,000 records across two 9,000-record retention loops, so it is the
# slowest test in the tree -- a few seconds, not instant. Output is line
# buffered, so if nothing has appeared at all then it really has stopped.
set -e
cd "$(dirname "$0")"

XPRS=../xprs_codec

# The codec's sha256 comes from mbedtls on the target. On the host it comes from
# test_sha256_host.c — the XPRS harness carries the same implementation, but
# inside a file that has a main() and so cannot be linked in here.
gcc -Wall -Wextra -Werror -O1 -DXPRSIDX_HOST_TEST \
    -I. -I"$XPRS" \
    -o /tmp/test_xprsindex \
    xprsindex.c "$XPRS"/xprs.c test_xprsindex_host.c test_sha256_host.c

/tmp/test_xprsindex
