# XPRS over BLE5 extended advertising — see xprsble.h and docs/ble5.md.
#
# The radio and nothing else: the queue, the rings and the §13.2.1 cancel are
# the caller's, the same division geogram_xprsnow keeps with geogram_xprsbearer.
idf_component_register(
    SRCS "xprsble.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_timer log
             tinynimble)
