# Generic LVGL status UI for XPRS stations: any board with an RGB565 panel
# supplies its size and a flush callback (see xprs_ui.h). LVGL arrives from
# the component registry via idf_component.yml.
idf_component_register(
    SRCS "xprs_ui.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls log lvgl__lvgl esp_timer
)
