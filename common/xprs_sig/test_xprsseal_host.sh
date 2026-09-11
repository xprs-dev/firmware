#!/bin/sh
# Host test for opening sealed bodies: built with OpenSSL for the curve and
# AES, and checked against bodies reticulum-dart sealed (tool/
# gen_seal_vectors.dart). xprssig.c comes along for the ECDH.
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXPRSSIG_HOST_TEST \
    -I. -o /tmp/test_xprsseal xprssig.c xprsseal.c test_xprsseal_host.c -lcrypto
/tmp/test_xprsseal
