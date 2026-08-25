# Only build for ePaper board (which has this RTC)
if(CONFIG_XPRS_BOARD_EPAPER_1IN54)
    idf_component_register(
        SRCS "pcf85063.cpp"
        INCLUDE_DIRS "."
        REQUIRES xprs_i2c log
        PRIV_REQUIRES freertos driver
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
