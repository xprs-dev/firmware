# The station UI for a board with no screen: every xui_* call in
# xprs_ui_api (xprs_ui.h) answered, none of them drawing anything, and no
# LVGL. A headless board names this one in its REQUIRES, as a screen board
# names xprs_ui, xprs_ui_mini or xprs_ui_paper; see xprs_ui_api.
idf_component_register(
    SRCS "xprs_ui_none.c"
    REQUIRES esp_timer xprs_ui_api)
