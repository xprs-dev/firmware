#!/bin/sh
# Host test for xprs_folder against the real XPRS codec.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
OUT="${TMPDIR:-/tmp}/test_xfolder"
gcc -Wall -Wextra -Werror -O1 -DXPRS_HOST_TEST -I. -I"$XPRS" \
    -o "$OUT" xprs_folder.c "$XPRS"/xprs.c "$XPRS"/xprs_sha256_sw.c test_xfolder_host.c
"$OUT"
