#!/bin/sh
# Host test for xprsid_verify: the 60- and 80-character signature forms against
# the specification's worked examples. OpenSSL stands in for mbedtls.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXPRSSIG_HOST_TEST \
    -I. -I../xprs_codec -I../xprs_sig \
    -o /tmp/test_xprsid xprsid.c ../xprs_sig/xprssig.c ../xprs_codec/xprs.c \
    test_xprsid_host.c -lcrypto
/tmp/test_xprsid
