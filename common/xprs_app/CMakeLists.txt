# The station itself. A board's own component needs only "xprs_app" in its
# REQUIRES -- everything below travels with it.
idf_component_register(
    SRCS "xprs_app.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log esp_timer esp_hw_support esp_wifi esp_event
             esp_netif lwip driver
             geogram_xprs xprs_station geogram_xprsbearer geogram_xprsnow
             geogram_xprslan geogram_nostr geogram_xprssig geogram_xprsid
             geogram_xprschan xprs_ui xprs_config esp_http_server
             geogram_xprsindex fatfs xprs_api xprs_hotspot espcoredump)
