# The board, and nothing else. The station is xprs_app, shared with every
# other board; the UI is xprs_ui_none, the one that draws nothing, because
# this board has no screen (and no room for LVGL).
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver log fatfs vfs wear_levelling xprs_app xprs_ui_none)
