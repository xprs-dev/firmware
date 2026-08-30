#!/bin/sh
# The ATT bytes, checked on a desk. See test_tn_att_host.c for why.
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
gcc -std=gnu99 -Wall -Wextra -Werror -O1 -I.. -o "$OUT/t" test_tn_att_host.c ../tn_att.c
"$OUT/t"
