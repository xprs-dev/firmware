#!/bin/sh
# The signing the page ships is tested where the page lives.
exec sh "$(dirname "$0")/../../../../portal/tool/test_crypto.sh"
