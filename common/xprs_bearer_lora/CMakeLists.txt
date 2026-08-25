idf_component_register(
    SRCS "xprslora.c"
    INCLUDE_DIRS "."
    REQUIRES xprs_codec xprs_bearer xprs_sx1262 log esp_timer
    PRIV_REQUIRES freertos esp_hw_support
)
