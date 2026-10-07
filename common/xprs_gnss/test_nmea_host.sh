#!/bin/sh
# Host-side test for the NMEA parser (nmea.c). Platform-free.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -o /tmp/test_nmea nmea.c test_nmea_host.c
/tmp/test_nmea
