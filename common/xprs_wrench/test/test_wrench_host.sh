#!/bin/sh
# Does the vendored VM still cost what CMakeLists.txt claims it costs?
#
# The whole case for putting a script VM on this board is a measured number:
# 29,232 bytes of .text with WRENCH_WITHOUT_COMPILER + WRENCH_COMPACT, and no
# libstdc++. Both are properties of the BUILD, not of the source, and nothing
# else in the tree would notice if either quietly stopped being true --
# exactly how a whole block of sdkconfig went missing for months
# (docs/esp32.md, "The reclaim went missing, and nothing said so").
#
# So it is checked on the desk:
#
#   1. no libstdc++. One upstream bump that adds an `operator new` or lets an
#      exception escape drags in ~40 KB of locale and unwind tables. That is
#      how the legacy multiboard image ended up carrying 318 KB of libstdc++
#      for a std::string it never asked for.
#   2. the compiler really is gone. If WRENCH_WITHOUT_COMPILER ever stops
#      taking effect the VM triples in size and still works, so nothing fails.
#   3. the size, printed, and compared against the target toolchain when it
#      is available -- the host figure is indicative, the Xtensa one is real.
set -e
cd "$(dirname "$0")"

DEFS="-DWRENCH_WITHOUT_COMPILER -DWRENCH_COMPACT -DWRENCH_TIME_SLICES
      -DWRENCH_HANDLE_MALLOC_FAIL -DWRENCH_PROTECT_STACK_FROM_OVERFLOW
      -DWRENCH_TRAP_DIVISION_BY_ZERO -DWRENCH_DEFAULT_STACK_SIZE=48"
FLAGS="-Os -fno-exceptions -fno-rtti -fno-threadsafe-statics
       -Wno-unused-parameter -Wno-sign-compare"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

fail() { echo "FAIL: $1"; exit 1; }

echo "== host build =="
g++ $FLAGS $DEFS -I.. -c ../wrench.cpp -o "$OUT/wrench_host.o"

# 1. Nothing from the C++ runtime. _Zn* = operator new, _Zd* = operator
#    delete, __cxa_* = ABI/exceptions, _Unwind_* = the unwinder.
if nm -u "$OUT/wrench_host.o" | grep -qE ' (_Zn|_Zd|__cxa_|_Unwind_)'; then
    nm -u "$OUT/wrench_host.o" | grep -E ' (_Zn|_Zd|__cxa_|_Unwind_)'
    fail "wrench pulled in libstdc++ -- see note 1 above"
fi
echo "   no libstdc++ symbols"

# 2. wr_compile still EXISTS with the compiler removed -- upstream leaves a
#    stub that returns WR_ERR_compiler_not_loaded -- so presence proves
#    nothing. Its size does: the stub is 7 bytes, the real entry point 108.
CSIZE=$(nm --print-size --radix=d "$OUT/wrench_host.o" \
        | awk '/ T .*wr_compilePKci/ {print $2 + 0; exit}')
[ -n "$CSIZE" ] || fail "wr_compile not found at all -- did the ABI change?"
[ "$CSIZE" -lt 32 ] || \
    fail "wr_compile is $CSIZE bytes, the stub is 7 -- WRENCH_WITHOUT_COMPILER did not take"
echo "   compiler absent (wr_compile is a ${CSIZE}-byte stub)"

# 3. The number that the plan is built on.
XT=$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++
if [ -x "$XT" ]; then
    echo "== xtensa-esp32s3 build (the figure that matters) =="
    "$XT" $FLAGS $DEFS -I.. -c ../wrench.cpp -o "$OUT/wrench_xt.o"
    "${XT%g++}size" "$OUT/wrench_xt.o" | tail -1 | \
        awk '{printf "   .text %d  .data %d  .bss %d  (budgeted: 31809 / 412 / 24)\n", $1, $2, $3}'
    TEXT=$("${XT%g++}size" "$OUT/wrench_xt.o" | tail -1 | awk '{print $1}')
    # A ceiling, not an equality: a toolchain bump moves this a little and
    # that is fine. A doubling means a define stopped taking effect.
    [ "$TEXT" -lt 40000 ] || fail ".text is ${TEXT}, budget is 29,232 and the ceiling is 40,000"
    echo "   within budget"
else
    echo "   (xtensa toolchain not installed -- skipped the real measurement)"
fi

echo "PASS: wrench builds small, with no C++ runtime and no compiler"
