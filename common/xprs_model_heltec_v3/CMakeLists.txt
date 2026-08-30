# Only the source is board-gated. REQUIRES/INCLUDE_DIRS are UNCONDITIONAL
# because ESP-IDF expands component requirements before Kconfig is loaded; a
# guarded REQUIRES is dropped from the dependency graph (then model_init.c
# cannot find ssd1306.h). See xprs_st7735/CMakeLists.txt.
if(CONFIG_XPRS_BOARD_HELTEC_V3)
    set(srcs "model_init.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES driver log nvs_flash esp_adc xprs_i2c xprs_ssd1306
    PRIV_REQUIRES freertos
)
