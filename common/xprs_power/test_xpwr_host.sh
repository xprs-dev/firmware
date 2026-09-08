#!/bin/sh
# Host-side test for the battery gauge.
#
# A learning gauge is the worst possible thing to debug on hardware: every
# question costs a full charge and half a day, and can be asked once. The
# arithmetic is a small struct and a clock, so it runs here instead.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXPWR_HOST_TEST -I. \
    -o /tmp/test_xpwr xprs_power.c test_xpwr_host.c
/tmp/test_xpwr
