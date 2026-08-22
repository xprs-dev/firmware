#!/bin/sh
# The mark's geometry, checked without a panel: the fit, the pen, the
# flattening and the refusals. A wrong scale is a number here and a shrug
# on a screenshot.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -Istubs -I.. \
    -o /tmp/test_xart test_xart_host.c ../xprs_art.c
/tmp/test_xart
