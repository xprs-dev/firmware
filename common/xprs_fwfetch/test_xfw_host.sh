#!/bin/sh
# Host test for the firmware fetch state machine (xfw.c), with every outside
# effect faked and a real publisher key signing the descriptions.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}"
gcc -O1 -c ../xprs_rns/tweetnacl.c -o "$OUT/tweetnacl_host.o"
gcc -Wall -Wextra -Werror -O1 -DRNS_HOST_TEST -DXPRSSIG_HOST_TEST -DXPRS_HOST_TEST \
    -I. -I../xprs_codec -I../xprs_id -I../xprs_sig -I../xprs_folder -I../xprs_rns \
    -o "$OUT/test_xfw" xfw.c ../xprs_folder/xprs_folder.c ../xprs_rns/rns.c \
    ../xprs_rns/rns_link.c ../xprs_rns/rns_resource.c ../xprs_id/xprsid.c \
    ../xprs_sig/xprssig.c ../xprs_codec/xprs.c ../xprs_codec/xprs_sha256_sw.c test_xfw_host.c "$OUT/tweetnacl_host.o" -lcrypto
"$OUT/test_xfw"
