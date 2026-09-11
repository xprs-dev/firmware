#!/bin/sh
# Host-side test for XPRS.md 11.10's words (xprs_setup.h): sealed lines and
# the values each setup key accepts. xprs_tz comes along for the offsets.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXTZ_HOST_TEST -I. -I../xprs_tz \
    -o /tmp/test_xprs_setup xprs_setup.c ../xprs_tz/xprs_tz.c test_xprs_setup_host.c
/tmp/test_xprs_setup
