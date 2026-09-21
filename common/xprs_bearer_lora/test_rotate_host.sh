#!/bin/sh
# Host-side test for the rotation's decision (lr_rotate.c). No radio, no
# ESP-IDF: the file is platform-free so that this can exist.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -o /tmp/test_rotate \
    lr_rotate.c test_rotate_host.c
/tmp/test_rotate
