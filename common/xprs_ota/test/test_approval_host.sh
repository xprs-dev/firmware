#!/bin/sh
# Does the DEVICE's verifier accept an approval the SIGNING TOOL produced?
#
# The two implementations are independent -- Dart in
# xprs-flutter/tool/sign_firmware.dart, C in common/geogram_xprssig -- and
# they only have to agree about one thing in the world: the digest of the
# xprsfw1 line. If they ever disagree, every station refuses every update
# and the fleet is unreachable, so this is checked on the desk.
#
# The vector below was produced by:
#   dart run tool/sign_firmware.dart --board m5stack-core --version 0.1.0 \
#       --bin <the 1,457,376-byte m5stack image> --nsec-file <a test key>
set -e
cd "$(dirname "$0")"
gcc -Wall -Wextra -Werror -O1 -DXPRSSIG_HOST_TEST \
    -I../../geogram_xprssig -o /tmp/test_xota_approval \
    test_approval_host.c ../../geogram_xprssig/xprssig.c -lcrypto
/tmp/test_xota_approval
