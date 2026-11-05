#!/bin/sh
# Host-side test for xapi_jesc: UTF-8 through, anything else escaped.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -o /tmp/test_jesc xapi_jesc.c test_jesc_host.c
/tmp/test_jesc
