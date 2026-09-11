#!/bin/sh
# Host-side test for the time-zone answers (xprs_tz.h): the two services'
# JSON and the offsets that are easy to get wrong. No network.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXTZ_HOST_TEST -I. \
    -o /tmp/test_xprs_tz xprs_tz.c test_xprs_tz_host.c
/tmp/test_xprs_tz
