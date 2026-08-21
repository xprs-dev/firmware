# Wrench 7.2.2 (MIT, see license.txt and README.md) -- the bytecode VM only.
#
# WRENCH_WITHOUT_COMPILER is not an optimisation, it is the design: upstream's
# own header says the compiler "is not particularly memory or space efficient,
# for small embedded systems it is strongly reccommeded (nay, required) that
# only bytecode be executed". Scripts are compiled on a desk by tools/wrenchc
# and shipped as signed bytecode.
#
# Measured on THIS toolchain (xtensa-esp32s3-elf-g++ -Os -fno-exceptions
# -fno-rtti), .text / .data / .bss:
#
#     with the compiler        94,503 / 3,176 / 28
#     WITHOUT_COMPILER         41,524 / 1,868 / 16
#     + COMPACT                29,232 /   412 / 16
#     + the four safety defines below
#                              31,809 /   412 / 24   <- what is built here
#     (+ REALLY_COMPACT        27,384 /   412 / 16, not taken -- see below)
#
# The safety options are not free: they cost 2,577 bytes over bare COMPACT.
# That is the honest price of a script that cannot hold the core, cannot
# dereference a failed allocation, and cannot silently divide by zero, and it
# is worth it. test/test_wrench_host.sh holds the build to it.
#
# REALLY_COMPACT is deliberately NOT taken: it saves 1,848 bytes of the 29,232
# by replacing the jump-table interpreter with a switch, and interpreter speed
# is the thing a time-sliced VM sharing core 1 has least of.
idf_component_register(
    SRCS "wrench.cpp"
    INCLUDE_DIRS "."
    REQUIRES log)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    # No on-device compiler. Bytecode only.
    WRENCH_WITHOUT_COMPILER
    # Smallest practical VM. See the table above.
    WRENCH_COMPACT
    # A script must not be able to hold core 1. Without this an infinite loop
    # in a third-party script is a watchdog reboot instead of a dropped module.
    WRENCH_TIME_SLICES
    # The script pool is capped and CAN refuse. Without this a refusal is a
    # null dereference rather than a clean unwind; upstream warns it costs an
    # if() on every instruction, and that is the right trade here.
    WRENCH_HANDLE_MALLOC_FAIL
    # Catches runaway recursion in the VM's own value stack. It does NOT
    # protect the C stack -- wr_callFunction is one 7,964-byte function and
    # upstream issue #54 is an instruction-fetch fault from deep recursion --
    # so the task stack is still sized by measurement, not by hope.
    WRENCH_PROTECT_STACK_FROM_OVERFLOW
    # A script dividing by zero should be a reported error, not a silent 0.
    WRENCH_TRAP_DIVISION_BY_ZERO
    # 256 entries x 8 bytes = 2 KB per context, and it comes out of the
    # script's PSRAM pool, not internal RAM.
    #
    # 48 was tried first and is too small to be useful: a script recursing to
    # depth 32 hit WR_ERR_stack_overflow. That failure was CLEAN -- error
    # returned, task alive, station still answering -- which is exactly what
    # WRENCH_PROTECT_STACK_FROM_OVERFLOW is for, and is why that define stays
    # on. But a limit a plausible script trips over is a limit set wrong.
    WRENCH_DEFAULT_STACK_SIZE=256)
