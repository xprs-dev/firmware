# XPRS over BLE5 extended advertising — see xprsble.h and docs/ble5.md.
#
# The radio, plus xprs_bearer's queue: this bearer now RELAYS (§13.1, "repeats
# a packet on the medium it heard it"), so it owns a re-air queue, the dedup
# rings and the §13.2.1 cancel like lan, lora and espnow do. Delivery to the
# application still goes straight out of handle_ad with its RSSI, so the
# station sees every advert exactly as before.
idf_component_register(
    SRCS "xprsble.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_timer log
             xprs_bearer xprs_codec
             tinynimble)
