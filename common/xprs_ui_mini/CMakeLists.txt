# The XPRS station UI for tiny screens (160x80 and up): any board with an
# RGB565 panel supplies its size and a flush callback (see xprs_ui_mini.h).
# LVGL arrives from the component registry via idf_component.yml.
idf_component_register(
    SRCS "xprs_ui_mini.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls log lvgl__lvgl esp_timer
)
