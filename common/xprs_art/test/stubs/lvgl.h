/* Just enough LVGL for the geometry to compile on a desk. */
#ifndef LVGL_STUB_H
#define LVGL_STUB_H
#include <stdint.h>
#include <stddef.h>
typedef int16_t lv_coord_t;
typedef struct { lv_coord_t x, y; } lv_point_t;
typedef struct { uint8_t r, g, b; } lv_color_t;
static inline lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b)
{ lv_color_t c = { r, g, b }; return c; }
#endif
