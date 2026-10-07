#!/bin/sh
# Host-side test for the network detection (lr_detect.c), its probe
# (lr_probe.c), the repeat-only flood (lr_repeat.c) and the airtime rule (lr_worth.c). Platform-free, like
# lr_rotate: no radio, no ESP-IDF. The Meshtastic and MeshCore codecs are
# linked for the probe's frames; the ciphers they reference are stubbed
# (test_detect_host.c), since a probe is not encrypted.
set -e
cd "$(dirname "$0")"
XPRS=../xprs_codec
XLC=../xprs_loracrypto
MT=../xprs_meshtastic
MC=../xprs_meshcore
SHA=../xprs_index/test_sha256_host.c
gcc -Wall -Wextra -Werror -O1 -I. -I"$XPRS" -I"$XLC" -I"$MT" -I"$MC" -o /tmp/test_detect \
    lr_detect.c lr_probe.c lr_repeat.c lr_worth.c test_detect_host.c \
    "$MT"/mt_wire.c "$MT"/mt_pki.c "$MC"/mc_wire.c "$XPRS"/xprs.c \
    "$XLC"/xlc_x25519.c "$XLC"/xlc_ed25519.c "$XLC"/xlc_hmac.c "$XLC"/xlc_sha512_sw.c "$SHA"
/tmp/test_detect
