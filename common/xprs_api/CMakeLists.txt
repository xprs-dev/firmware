# The station's HTTP API, board-agnostic (spec/API-HTTP.md). Boards supply
# their callsign, index, transmitter and switches through xprs_api_cfg_t.
idf_component_register(
    SRCS "xprs_api.c" "xapi_send.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server geogram_xprsindex geogram_xprs esp_timer log
             xprs_auth xprs_ota app_update esp_app_format espcoredump esp_system
)
