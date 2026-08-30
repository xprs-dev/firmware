# The CONDENSED implementation of the station UI: three rotating views on a
# strip (160x80 and up), for a board the seven-panel xprs_ui refuses. Any
# such board supplies its size and a flush callback (see xprs_ui_mini.h).
#
# Two files, and the split matters. xprs_ui_mini.c is the screen that was
# drawn for the T-Dongle and is unchanged by any of this; xui_mini.c is the
# adapter that makes it answer to the shared interface (xprs_ui_api), so
# xprs_app drives this board without knowing it is not the big dashboard.
#
# A board links this OR xprs_ui, never both. LVGL arrives from the component
# registry via idf_component.yml.
idf_component_register(
    SRCS "xprs_ui_mini.c" "xui_mini.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls log lvgl__lvgl esp_timer xprs_ui_api
)
