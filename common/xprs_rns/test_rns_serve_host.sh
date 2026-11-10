#!/bin/sh
# Host test for the request server (rns_serve.c), the link responder
# (rns_link.c) and msgpack (rns_msgpack.c), against the real reticulum-dart
# client (tool/request_client.dart) run as a child process. Needs the
# Flutter SDK's dart (DART=~/flutter/bin/dart) and the reticulum-dart
# checkout next to this repo. On the shared Pi: `xbuild sh test_rns_serve_host.sh`.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}"
gcc -O1 -c ./tweetnacl.c -o "$OUT/tweetnacl_serve.o"
gcc -Wall -Wextra -Werror -O1 -g -DRNS_HOST_TEST -DRNS_RES_CACHE_PARTS=16 -I. \
    -o "$OUT/test_rns_serve" \
    rns.c rns_link.c rns_resource.c rns_msgpack.c rns_serve.c test_rns_serve_host.c \
    "$OUT/tweetnacl_serve.o" -lcrypto
DART="${DART:-$HOME/flutter/bin/dart}" "$OUT/test_rns_serve" "$(cd ../../../reticulum-dart && pwd)/tool/request_client.dart"
