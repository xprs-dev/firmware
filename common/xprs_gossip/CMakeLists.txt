# Gossip: who has been heard where, and by whom (XPRS.md 36.9.4) — see
# xgossip.h. Like xprs_index, it is stdio over whatever VFS the caller's
# directory lives on, so it registers on every target and stays dead code
# where xgossip_open() is never called.
#
# test_xgossip_host.c is a HOST gcc harness (run test_xgossip_host.sh) and is
# deliberately not listed in SRCS — the same arrangement as xprs_index.
idf_component_register(
    SRCS "xgossip.c"
    INCLUDE_DIRS "."
    REQUIRES log)
