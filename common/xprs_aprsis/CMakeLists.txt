idf_component_register(
    SRCS "aprsis.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_timer lwip freertos xprs_wifi xprs_msgstore
)
