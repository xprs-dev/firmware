/**
 * @file xprs_art.h
 * @brief The Geogram mark, as strokes a panel can draw.
 *
 * spec/artwork/splash/geogram-triad-dark.svg is nothing but ten stroked
 * shapes -- four lines for the star, three chevrons, three waves -- so it
 * needs no SVG renderer and no bitmap. It needs a coordinate table and
 * lv_line, which xprs_ui already uses for the radar's rings and sweep.
 *
 * The alternative was a 320x240 RGB565 blob: 153,600 bytes in a partition,
 * and useless on the T-Dongle's 160x80 panel. This is 212 bytes of .rodata
 * that fits any box on any board.
 *
 * NOTHING HERE ALLOCATES OR TOUCHES LVGL STATE. xart_build() fills arrays
 * the caller owns; the caller makes the objects. That keeps the geometry
 * testable on a desk and keeps this file free of the UI's lifetime rules.
 */
#ifndef XPRS_ART_H
#define XPRS_ART_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The three signs. The T-Dongle's 160x80 has room for the star alone. */
#define XART_G_STAR   0x01
#define XART_G_CHEV   0x02
#define XART_G_WAVE   0x04
#define XART_G_TRIAD  (XART_G_STAR | XART_G_CHEV | XART_G_WAVE)

/* Worst case, so a caller can size its arrays statically and never ask. */
#define XART_PTS_TRIAD   56
#define XART_POLY_TRIAD  10
#define XART_PTS_STAR     8
#define XART_POLY_STAR    4

/* spec/artwork/README.md: ink, bone, dark, sand. */
#define XART_BONE  lv_color_make(0xE8, 0xE3, 0xD6)
#define XART_INK   lv_color_make(0x2A, 0x26, 0x20)
#define XART_DARK  lv_color_make(0x23, 0x2A, 0x2E)
#define XART_SAND  lv_color_make(0xF4, 0xF1, 0xEA)

/** One polyline: a run of points inside the caller's array. */
typedef struct {
    const lv_point_t *pts;
    uint16_t          n;
} xart_poly_t;

/**
 * Fit @p groups into the box (@p x, @p y, @p w, @p h) and emit polylines.
 *
 * Points are written to @p pts in the coordinate space of whatever object
 * the caller will parent the lines to, so each polyline becomes one lv_line
 * placed at the origin -- the radar's idiom (xprs_ui.c, s_cross_pts).
 *
 * lv_line DOES NOT COPY ITS POINTS: @p pts must outlive every object built
 * from @p out. @p out is read immediately and may be a local.
 *
 * @param stroke  out: the pen width in pixels, per the brand's rule that a
 *                mark inked at 7.5% of its box at large sizes thickens to
 *                15% when it is small enough to lose its strokes.
 * @return polylines written, or -1 if either capacity is short.
 */
int xart_build(uint8_t groups, int x, int y, int w, int h,
               lv_point_t *pts, int pts_cap,
               xart_poly_t *out, int out_cap, int *stroke);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_ART_H */
