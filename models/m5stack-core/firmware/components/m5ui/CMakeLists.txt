# Board-local LVGL layer for the M5Stack Core. No board-symbol gating: only
# this project compiles it. LVGL arrives from the component registry via
# idf_component.yml, same arrangement as the T-Dongle's UI component.
idf_component_register(
    SRCS "m5ui.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls log ili9342 lvgl__lvgl esp_timer
)
