#!/bin/sh
# Host test for xwc: the web chat's station API rules (XPRS.md 10.10),
# the scenarios the Dart core's WebChatApi tests use. OpenSSL stands in
# for mbedtls. On the shared Pi run it as `xbuild sh test_xwc_host.sh`.
set -e
cd "$(dirname "$0")"
# `sh test_xwc_host.sh san` adds the address and undefined-behaviour
# sanitizers (an argument, not an environment variable: xbuild keys its
# reuse on the command text). An overflow in the engine shows here as a
# report; on the board it is a watchdog reset with nothing printed, the
# heap's lock spinning with interrupts off. Where ASan cannot reserve its
# shadow (this Pi), `valgrind /tmp/test_xwc` after a plain build does.
SAN=${1:-}
cc -Wall -Wextra -Werror -O1 -g -DXPRSSIG_HOST_TEST \
    ${SAN:+-fsanitize=address,undefined -fno-omit-frame-pointer} \
    -I. -I../xprs_codec -I../xprs_id -I../xprs_sig \
    -o /tmp/test_xwc${SAN:+_san} xwc.c ../xprs_id/xprsid.c ../xprs_sig/xprssig.c \
    ../xprs_codec/xprs.c test_xwc_host.c -lcrypto
/tmp/test_xwc${SAN:+_san}
