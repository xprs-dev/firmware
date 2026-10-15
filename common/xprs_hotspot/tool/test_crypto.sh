#!/bin/sh
# The signing the page ships is tested where the page lives.
exec sh "$(dirname "$0")/../../../../webchat/tool/test_crypto.sh"
