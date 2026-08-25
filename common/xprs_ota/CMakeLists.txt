# Installing firmware without a ladder (XPRS.md 25.8). Plain HTTP on
# purpose -- the signature authenticates the bytes, not the carrier; see
# the header. The board supplies its id, its callsign, a way to air an
# answer and a way to quiesce its storage.
idf_component_register(
    SRCS "xprs_ota.c" "xota_http.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server xprs_auth esp_https_ota esp_http_client app_update esp_app_format
             mbedtls nvs_flash esp_timer log
             xprs_sig xprs_config)
