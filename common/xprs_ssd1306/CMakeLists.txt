# Only the source is board-gated (the SSD1306 OLED is on the Heltec boards).
# REQUIRES/INCLUDE_DIRS are UNCONDITIONAL: ESP-IDF expands component
# requirements before Kconfig is loaded, so a guarded REQUIRES is dropped from
# the dependency graph and nothing that includes ssd1306.h can find it. See
# xprs_st7735/CMakeLists.txt for the full explanation.
if(CONFIG_XPRS_BOARD_HELTEC_V1 OR CONFIG_XPRS_BOARD_HELTEC_V2 OR CONFIG_XPRS_BOARD_HELTEC_V3)
    set(srcs "ssd1306.c")
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES driver log xprs_i2c
    PRIV_REQUIRES freertos
)
