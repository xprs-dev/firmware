#!/bin/sh
# Host-side test for the classifier (lr_class.c). No radio, no ESP-IDF and
# no crypto: the file takes constants from mt.h and mc.h and reads bytes,
# which is what makes this harness one line long.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -I../xprs_meshtastic -I../xprs_meshcore \
    -o /tmp/test_class lr_class.c test_class_host.c
/tmp/test_class
