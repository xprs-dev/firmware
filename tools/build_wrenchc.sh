#!/bin/sh
# Build the host-side Wrench compiler from the SAME vendored source the
# firmware runs, so the compiler and the VM can never be different versions.
set -e
cd "$(dirname "$0")"
SRC=../common/geogram_wrench
g++ -O2 -std=gnu++11 -fno-exceptions -fno-rtti \
    -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
    -I"$SRC" -o wrenchc wrenchc.cpp "$SRC/wrench.cpp"
echo "built: $(pwd)/wrenchc  (wrench $(grep -m1 WRENCH_VERSION_MAJOR "$SRC/wrench.h" | tr -dc 0-9).$(grep -m1 WRENCH_VERSION_MINOR "$SRC/wrench.h" | tr -dc 0-9).$(grep -m1 WRENCH_VERSION_BUILD "$SRC/wrench.h" | tr -dc 0-9))"
