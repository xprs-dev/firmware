# LVGL port for XPRS project
# Only build for ESP32-S3 targets with e-paper display

if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    idf_component_register(
        SRCS "lvgl_port.c"
        INCLUDE_DIRS "."
        REQUIRES lvgl xprs_epaper_1in54 log nvs_flash
        PRIV_REQUIRES freertos esp_timer heap
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
