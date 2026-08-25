idf_component_register(
    SRCS "ble_hello.c" "ble_parcel.c"
    INCLUDE_DIRS "."
    REQUIRES log bt nvs_flash json esp_timer xprs_msgstore xprs_index xprs_codec
)
