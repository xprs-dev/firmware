#!/bin/sh
# Host-side test for xprs_meshtastic: the wire, the XPRS framing, and the
# repeater/bridge driven by two fake bridges and one fake Meshtastic node.
# AES comes from the host's OpenSSL (libcrypto), SHA-256 from the same
# host shim the bearer tests use.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -I. -I"$XPRS" -o /tmp/test_mt \
    mt_wire.c mt_xprs.c mt_mesh.c mt_pki.c "$XPRS"/xprs.c \
    test_mt_host.c test_mt_mesh_host.c "$SHA" -lcrypto
/tmp/test_mt
