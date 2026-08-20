idf_component_register(
    SRCS "xprslora.c"
    INCLUDE_DIRS "."
    REQUIRES geogram_xprs geogram_xprsbearer geogram_sx1262 log esp_timer
    PRIV_REQUIRES freertos esp_hw_support
)
