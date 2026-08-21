#!/bin/sh
# The HCI bytes, checked on a desk. See test_tn_host.c for why.
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
gcc -std=gnu99 -Wall -Wextra -Werror -O1 -I.. -o "$OUT/t" test_tn_host.c ../tn_hci.c
"$OUT/t"
