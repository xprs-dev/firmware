#!/bin/sh
# Host test for the link initiator and the streamed resource receiver, against
# the real reticulum-dart responder (tool/files_responder.dart) run as a child
# process. Needs the Flutter SDK's dart (DART=~/flutter/bin/dart) and the
# reticulum-dart checkout next to this repo.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}"
gcc -O1 -c ./tweetnacl.c -o "$OUT/tweetnacl_host.o"
gcc -Wall -Wextra -Werror -O1 -DRNS_HOST_TEST -DRNS_RES_CACHE_PARTS=16 -I. \
    -o "$OUT/test_rns_link" \
    rns.c rns_link.c rns_resource.c test_rns_link_host.c "$OUT/tweetnacl_host.o" -lcrypto
DART="${DART:-$HOME/flutter/bin/dart}" "$OUT/test_rns_link" "$(cd ../../../reticulum-dart && pwd)/tool/files_responder.dart"
