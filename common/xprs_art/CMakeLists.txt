# The Geogram mark as strokes -- see xprs_art.h. Depends on LVGL only for
# lv_point_t and lv_color_t; it draws nothing itself.
idf_component_register(
    SRCS "xprs_art.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl)
