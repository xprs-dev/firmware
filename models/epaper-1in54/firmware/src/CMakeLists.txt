# The board, and nothing else. The station is xprs_app, shared with every
# other board; what is named here beyond it is this board's own panel and
# sensor code (in this directory), the shared I2C helper, and the ONE UI
# implementation drawn for e-paper, xprs_ui_paper. Naming a second UI would
# be a duplicate symbol for every xui_* call (common/xprs_ui_api).
idf_component_register(
    SRCS "main.c" "epd.c" "shtc3.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_adc esp_timer fatfs vfs sdmmc wear_levelling nvs_flash
             xprs_app xprs_ui_paper xprs_i2c)
