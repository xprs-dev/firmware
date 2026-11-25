#!/bin/sh
# Host test for the neighbours' clock hint (xauth_hint.c), alone: it has no
# dependencies beyond xprs_auth.h's types.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -I. -I../xprs_codec -o /tmp/test_xauth_hint \
    xauth_hint.c test_xauth_hint_host.c
/tmp/test_xauth_hint
