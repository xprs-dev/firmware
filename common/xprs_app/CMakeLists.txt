# The station itself. A board's own component needs "xprs_app" plus ONE UI
# implementation -- xprs_ui or xprs_ui_mini -- in its REQUIRES; everything
# else below travels with it.
#
# The UI is named here only by its interface (xprs_ui_api, the header), so
# that this component compiles once and links against whichever of the two
# the board chose. See common/xprs_ui_api/CMakeLists.txt.
idf_component_register(
    SRCS "xprs_app.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log esp_timer esp_hw_support esp_wifi esp_event
             esp_netif lwip driver
             xprs_codec xprs_station xprs_bearer xprs_bearer_now
             xprs_bearer_lan xprs_bearer_lora xprs_bearer_ble xprs_nostr xprs_sig xprs_id
             xprs_chan xprs_ui_api xprs_config esp_http_server
             xprs_index xprs_gossip xprs_cadence fatfs xprs_api xprs_hotspot espcoredump
             xprs_auth xprs_ota xprs_health xprs_diag xprs_common
             xprs_bearer_rns)
