#!/bin/sh
# Host-side test for the hourly limits on other people's mail (xmailcap.c).
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -o ${TMPDIR:-/tmp}/test_xmailcap xmailcap.c test_xmailcap_host.c
${TMPDIR:-/tmp}/test_xmailcap
