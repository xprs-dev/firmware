# Only build for ESP32-S3 ePaper board
if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    idf_component_register(
        SRCS "model_init.c" "board_power.cpp"
        INCLUDE_DIRS "."
        REQUIRES driver log nvs_flash xprs_i2c xprs_epaper_1in54 xprs_pcf85063 xprs_shtc3 xprs_button xprs_sdcard
        PRIV_REQUIRES freertos
    )
else()
    idf_component_register()
endif()
