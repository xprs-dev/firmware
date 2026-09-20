idf_component_register(
    SRCS "xprslora.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_bearer xprs_sx1262 xprs_meshtastic xprs_meshcore xprs_common log esp_timer
    PRIV_REQUIRES freertos esp_hw_support heap nvs_flash
)
