# The board, and nothing else. The station is xprs_app, shared with the
# T-Dongle, the T-Deck and the M5Stack; what is named here beyond it is this
# board's screen (xprs_ssd1306 over xprs_i2c, on the pins in
# xprs_model_heltec_v3) and the ONE UI implementation a 128x64 panel can use
# -- xprs_ui_mini, not xprs_ui, which refuses anything under 160x120. Naming
# both would be a duplicate symbol for every xui_* call; see
# common/xprs_ui_api/CMakeLists.txt.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_adc xprs_app xprs_ui_mini
             xprs_model_heltec_v3 xprs_ssd1306 xprs_i2c)
