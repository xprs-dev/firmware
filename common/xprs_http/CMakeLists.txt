# XPRS HTTP server component for WiFi configuration and Station API

# Base requirements for all boards. xprs_msgstore is listed unconditionally
# (it registers as an empty component on boards without an SD slot); component
# REQUIRES are resolved before CONFIG_* is applied, so a CONFIG-guarded require
# is unreliable. http_server.c only includes/uses it under MODEL_TDONGLE_S3.
set(HTTP_REQUIRES esp_http_server esp_https_server nvs_flash log xprs_node xprs_ws xprs_common xprs_msgstore)

# Add tiles and updates for boards with SD card support
if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    list(APPEND HTTP_REQUIRES xprs_tiles xprs_updates)
endif()

# Private requirements — APRS/SA818/model_kv4p are empty stubs on non-KV4P boards
set(HTTP_PRIV_REQUIRES esp_wifi xprs_aprs xprs_sa818 xprs_model_kv4p xprs_radio_tx)

# Add mesh and nostr components on targets that support ESP-MESH
# These are used conditionally via CONFIG_XPRS_MESH_ENABLED
if("${IDF_TARGET}" STREQUAL "esp32" OR "${IDF_TARGET}" STREQUAL "esp32s2" OR "${IDF_TARGET}" STREQUAL "esp32s3" OR "${IDF_TARGET}" STREQUAL "esp32c3")
    list(APPEND HTTP_PRIV_REQUIRES xprs_mesh xprs_nostr)
endif()

idf_component_register(
    SRCS "http_server.c" "chat_page.c"
    INCLUDE_DIRS "."
    REQUIRES ${HTTP_REQUIRES}
    PRIV_REQUIRES ${HTTP_PRIV_REQUIRES}
)
