#!/bin/sh
# Host-side test for xprs_loracrypto: the curve and cipher primitives the
# LoRa networks share, each against OpenSSL or its RFC's own vector.
# The AES seam is filled by the harness (OpenSSL); SHA-512 by the software
# file a target without mbedtls would use, and SHA-256 by the same shim the
# other harnesses take.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -I. -I"$XPRS" -o /tmp/test_xlc \
    xlc_x25519.c xlc_ed25519.c xlc_hmac.c xlc_sha512_sw.c "$XPRS"/xprs.c \
    test_xlc_host.c "$SHA" -lcrypto
/tmp/test_xlc
