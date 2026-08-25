# Only build for Heltec WiFi LoRa 32 V1
if(CONFIG_XPRS_BOARD_HELTEC_V1)
    idf_component_register(
        SRCS "model_init.c"
        INCLUDE_DIRS "."
        REQUIRES driver log nvs_flash xprs_i2c xprs_ssd1306 xprs_sx1276
        PRIV_REQUIRES freertos
    )
else()
    idf_component_register()
endif()
