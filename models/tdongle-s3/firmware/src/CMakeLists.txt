idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bt tinynimble nvs_flash log esp_timer mbedtls esp_hw_support xprs_health
             esp_wifi esp_event esp_netif lwip driver
             xprs_model_tdongle_s3 xprs_st7735 xprs_station xprs_ui_mini
             xprs_config xprs_auth xprs_ota xprs_diag xprs_api app_update esp_app_format espcoredump
             xprs_wifi xprs_aprsis xprs_blemesh xprs_sdcard
             xprs_lanwatch xprs_codec xprs_index xprs_bearer_lan xprs_bearer xprs_bearer_now xprs_id xprs_chan xprs_rns xprs_sig xprs_nostr
             esp_http_server
)
