idf_component_register(
    SRCS "xprs_ble.c"
    INCLUDE_DIRS "."
    REQUIRES log bt nvs_flash json xprs_node xprs_mesh xprs_nostr geoblue
    PRIV_REQUIRES xprs_aprs xprs_radio_tx
)
