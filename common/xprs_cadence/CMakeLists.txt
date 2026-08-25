# The poll ladder of XPRS.md 36.10.2 — see xcadence.h. Pure arithmetic, no
# clock and no I/O, so it registers everywhere and costs nothing where it is
# not called. test_xcadence_host.c is a HOST gcc harness (run
# test_xcadence_host.sh) and is deliberately not in SRCS.
idf_component_register(SRCS "xcadence.c" INCLUDE_DIRS ".")
