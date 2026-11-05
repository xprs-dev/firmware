#!/bin/sh
# Host-side test for radar_layout: where the radar's contacts and names go.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -o /tmp/test_radar \
    radar_layout.c test_radar_host.c -lm
/tmp/test_radar
