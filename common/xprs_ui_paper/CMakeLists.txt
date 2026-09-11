# The e-paper implementation of the station UI: one black-and-white screen
# for a 200x200 panel (the Waveshare ESP32-S3-ePaper-1.54 is the reference).
# The interface it answers to is xprs_ui_api (xprs_ui.h); xprs_ui and
# xprs_ui_mini are the other two implementations, and a board links exactly
# one of the three. LVGL arrives from the component registry via
# idf_component.yml.
idf_component_register(
    SRCS "xprs_ui_paper.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl esp_timer esp_wifi log mbedtls heap xprs_ui_api xprs_station
)
