# The BIG implementation of the station UI: the seven-panel dashboard, for
# any board with a 240px-or-taller RGB565 panel. The interface it answers to
# is xprs_ui_api (xprs_ui.h); xprs_ui_mini is the other implementation, and a
# board links exactly one of the two. LVGL arrives from the component
# registry via idf_component.yml.
idf_component_register(
    SRCS "xprs_ui.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls log lvgl__lvgl esp_timer xprs_common xprs_art
             xprs_ui_api
)
