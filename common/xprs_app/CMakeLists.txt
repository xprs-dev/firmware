# The station itself. A board's own component needs only "xprs_app" in its
# REQUIRES -- everything below travels with it.
idf_component_register(
    SRCS "xprs_app.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log esp_timer esp_hw_support esp_wifi esp_event
             esp_netif lwip driver
             xprs_codec xprs_station xprs_bearer xprs_bearer_now
             xprs_bearer_lan xprs_bearer_lora xprs_bearer_ble xprs_nostr xprs_sig xprs_id
             xprs_chan xprs_ui xprs_config esp_http_server
             xprs_index xprs_gossip fatfs xprs_api xprs_hotspot espcoredump
             xprs_auth xprs_ota xprs_health xprs_diag xprs_common
             xprs_bearer_rns)
