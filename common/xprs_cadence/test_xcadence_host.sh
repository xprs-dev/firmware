#!/bin/sh
# Host-side test for the poll ladder of XPRS.md 36.10.2. Pure arithmetic, so
# it needs no board, no card and no radio.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -o /tmp/test_xcadence \
    xcadence.c test_xcadence_host.c
/tmp/test_xcadence
