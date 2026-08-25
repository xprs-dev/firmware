# XPRS UI component
# Only build for ESP32-S3 targets with e-paper display

if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    idf_component_register(
        SRCS "xprs_ui_epaper.c"
        INCLUDE_DIRS "."
        REQUIRES lvgl xprs_lvgl xprs_shtc3 log
        PRIV_REQUIRES freertos
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
