#!/bin/sh
# Host-side test for xprs_meshcore: the frame against MeshCore's published
# layout, the crypto against OpenSSL, and the XPRS framing against itself.
# The two AES seams come from the host's OpenSSL (libcrypto), SHA-512 from
# the software file, SHA-256 from the same shim the other harnesses take.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
XLC=../xprs_loracrypto
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -I. -I"$XPRS" -I"$XLC" -o /tmp/test_mc \
    mc_wire.c mc_xprs.c mc_mesh.c "$XPRS"/xprs.c \
    "$XLC"/xlc_x25519.c "$XLC"/xlc_ed25519.c "$XLC"/xlc_hmac.c \
    "$XLC"/xlc_sha512_sw.c \
    test_mc_host.c test_mc_mesh_host.c "$SHA" -lcrypto
/tmp/test_mc
