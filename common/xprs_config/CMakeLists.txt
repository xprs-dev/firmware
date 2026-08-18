# Station configuration: NVS-backed settings with a config.ini face and an
# opt-in HTTP editor (see xprs_config.h).
idf_component_register(
    SRCS "xprs_config.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_http_server esp_timer log
)
