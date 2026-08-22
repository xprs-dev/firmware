/**
 * @file xprs_art.c
 * @brief The mark's geometry (see xprs_art.h).
 */

#include "xprs_art.h"

/*
 * Transcribed from spec/artwork/splash/geogram-triad-dark.svg.
 *
 * That file's viewBox is "0 12 276 258", so twelve is subtracted from every
 * y HERE, once, and the table below is a plain 276x258 box starting at the
 * origin. Doing it at authoring time rather than at run time is the whole
 * reason this reads like the SVG when you diff the two by eye.
 *
 * The waves are written in the SVG as "Q c e T e2", where T's control point
 * is implied -- the reflection of the previous control about the previous
 * end, 2*P_end - C_prev. That reflection is worked out here too, so a wave
 * is simply five control points: P0 C0 P1 C1 P2. For the first wave,
 * 2*(208,163) - (180,146) = (236,180).
 */
static const int16_t k_xy[] = {
    /*  0- 1 */ 138,  12,  138, 104,                    /* star |          */
    /*  2- 3 */  92,  58,  184,  58,                    /* star -          */
    /*  4- 5 */ 105,  25,  171,  91,                    /* star \          */
    /*  6- 7 */ 171,  25,  105,  91,                    /* star /          */
    /*  8-10 */  44, 186,   68, 136,   92, 186,         /* chevron, upper  */
    /* 11-13 */  12, 248,   36, 198,   60, 248,         /* chevron, left   */
    /* 14-16 */  76, 248,  100, 198,  124, 248,         /* chevron, right  */
    /* 17-21 */ 152, 163,  180, 146,  208, 163,  236, 180,  264, 163,
    /* 22-26 */ 152, 198,  180, 181,  208, 198,  236, 215,  264, 198,
    /* 27-31 */ 152, 233,  180, 216,  208, 233,  236, 250,  264, 233,
};

#define XART_K_POLY 0   /* n vertices, joined in order                     */
#define XART_K_QUAD 1   /* n = 2q+1 control points: q chained quadratics   */

typedef struct { uint8_t group, kind, first, n; } xart_shape_t;

static const xart_shape_t k_shape[] = {
    { XART_G_STAR, XART_K_POLY,  0, 2 }, { XART_G_STAR, XART_K_POLY,  2, 2 },
    { XART_G_STAR, XART_K_POLY,  4, 2 }, { XART_G_STAR, XART_K_POLY,  6, 2 },
    { XART_G_CHEV, XART_K_POLY,  8, 3 }, { XART_G_CHEV, XART_K_POLY, 11, 3 },
    { XART_G_CHEV, XART_K_POLY, 14, 3 },
    { XART_G_WAVE, XART_K_QUAD, 17, 5 }, { XART_G_WAVE, XART_K_QUAD, 22, 5 },
    { XART_G_WAVE, XART_K_QUAD, 27, 5 },
};

/*
 * The box each group's INK occupies -- the drawn curve, not the control net.
 * Wave 3's control point sits at y=250 but the pen never goes below 241.5,
 * and fitting to the control would shove the whole mark up by eight units
 * for a place nothing is drawn. x0, y0, x1, y1, rounded outward.
 */
static const int16_t k_gbox[3][4] = {
    {  92,  12, 184, 104 },   /* STAR: square, 92 x 92 */
    {  12, 136, 124, 248 },   /* CHEV                  */
    { 152, 154, 264, 242 },   /* WAVE                  */
};

/*
 * spec/artwork/README.md gives the pen as a share of the ICON BOX, not of
 * the mark -- which is why both SVGs carry stroke-width 9: the star's box is
 * 120 units, and 9/120 is 7.5%. So the icon box is defined as 120 source
 * units here, and the table is read against the box as rendered.
 *
 * { dp, percent x 10 }
 */
static const int16_t k_pen[][2] = {
    { 18, 150 }, { 24, 130 }, { 32, 115 }, { 48, 90 }, { 64, 75 },
};

#define Q 12                      /* fixed point: 1.0 == 1 << 12 */
#define XART_QUAD_SEGS 6

/*
 * Six segments per quadratic. A chord's worst deviation from its curve is
 * |2C - P0 - P1| / 4, which for these waves is 8.5 source units, and
 * subdividing into N pieces divides that by N*N: 2.13 at N=2, 0.53 at N=4,
 * 0.236 at N=6. At the scale a 320-wide panel gives the triad that last is
 * about a seventh of a pixel -- invisible under a six-pixel round-capped
 * pen, and it stays honest if the mark is ever drawn four times bigger.
 */
static int pen_for_box(int box_px)
{
    const int n = (int)(sizeof k_pen / sizeof k_pen[0]);
    int pct10;
    if (box_px <= k_pen[0][0]) {
        pct10 = k_pen[0][1];
    } else if (box_px >= k_pen[n - 1][0]) {
        pct10 = k_pen[n - 1][1];
    } else {
        int i = 0;
        while (i + 1 < n && box_px > k_pen[i + 1][0]) i++;
        int x0 = k_pen[i][0], x1 = k_pen[i + 1][0];
        int y0 = k_pen[i][1], y1 = k_pen[i + 1][1];
        pct10 = y0 + (y1 - y0) * (box_px - x0) / (x1 - x0);
    }
    int pen = (box_px * pct10 + 500) / 1000;
    return pen < 1 ? 1 : pen;
}

/* Source-space bounding box of everything selected. */
static void groups_box(uint8_t groups, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = *y0 = 0x7FFF;
    *x1 = *y1 = -0x7FFF;
    for (int g = 0; g < 3; g++) {
        if (!(groups & (1u << g))) continue;
        if (k_gbox[g][0] < *x0) *x0 = k_gbox[g][0];
        if (k_gbox[g][1] < *y0) *y0 = k_gbox[g][1];
        if (k_gbox[g][2] > *x1) *x1 = k_gbox[g][2];
        if (k_gbox[g][3] > *y1) *y1 = k_gbox[g][3];
    }
}

int xart_build(uint8_t groups, int x, int y, int w, int h,
               lv_point_t *pts, int pts_cap,
               xart_poly_t *out, int out_cap, int *stroke)
{
    if (!groups || !pts || !out || w <= 0 || h <= 0) return -1;

    int sx0, sy0, sx1, sy1;
    groups_box(groups, &sx0, &sy0, &sx1, &sy1);
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0) return -1;

    /*
     * Two passes. The first sizes the mark as if the pen were infinitely
     * thin and asks how thick it should then be; the second re-fits inside
     * that margin, because a round cap puts half the pen outside every
     * endpoint and a mark fitted edge-to-edge loses it.
     *
     * The scale is taken from pass two but the pen from whichever pass
     * asked for less. A third pass would refine it further and, on a small
     * panel, oscillate between two widths forever; taking the smaller pen
     * with the tighter scale cannot clip, because the margin was cut for
     * the heavier one.
     */
    int32_t sq = 0;
    int pen = 0;
    for (int pass = 0; pass < 2; pass++) {
        int aw = w - 2 * pen, ah = h - 2 * pen;
        if (aw <= 0 || ah <= 0) return -1;
        int32_t qx = ((int32_t)aw << Q) / sw;
        int32_t qy = ((int32_t)ah << Q) / sh;
        sq = qx < qy ? qx : qy;
        int box = (int)((120 * sq) >> Q);        /* the icon box, rendered */
        int p = pen_for_box(box);
        pen = (pass == 0 || p < pen) ? p : pen;
    }
    if (stroke) *stroke = pen;

    int mw = (int)(((int32_t)sw * sq) >> Q);
    int mh = (int)(((int32_t)sh * sq) >> Q);
    int ox = x + pen + ((w - 2 * pen) - mw) / 2;
    int oy = y + pen + ((h - 2 * pen) - mh) / 2;

    #define MAPX(v) (lv_coord_t)(ox + (int)((((int32_t)(v) - sx0) * sq + (1 << (Q - 1))) >> Q))
    #define MAPY(v) (lv_coord_t)(oy + (int)((((int32_t)(v) - sy0) * sq + (1 << (Q - 1))) >> Q))

    int np = 0, no = 0;
    for (size_t s = 0; s < sizeof k_shape / sizeof k_shape[0]; s++) {
        const xart_shape_t *sh_ = &k_shape[s];
        if (!(groups & sh_->group)) continue;
        if (no >= out_cap) return -1;

        const int16_t *src = &k_xy[sh_->first * 2];
        int start = np;

        if (sh_->kind == XART_K_POLY) {
            if (np + sh_->n > pts_cap) return -1;
            for (int i = 0; i < sh_->n; i++) {
                pts[np].x = MAPX(src[i * 2]);
                pts[np].y = MAPY(src[i * 2 + 1]);
                np++;
            }
        } else {
            /*
             * Chained quadratics. Integer Bernstein weights over the common
             * denominator N*N, so no control point is ever rounded on the
             * way in and the curve is evaluated at better than source
             * resolution. The joint between two segments is emitted once.
             */
            int quads = (sh_->n - 1) / 2;
            int need = 1 + quads * XART_QUAD_SEGS;
            if (np + need > pts_cap) return -1;
            pts[np].x = MAPX(src[0]);
            pts[np].y = MAPY(src[1]);
            np++;
            for (int q = 0; q < quads; q++) {
                int32_t p0x = src[q * 4 + 0], p0y = src[q * 4 + 1];
                int32_t cx  = src[q * 4 + 2], cy  = src[q * 4 + 3];
                int32_t p1x = src[q * 4 + 4], p1y = src[q * 4 + 5];
                for (int i = 1; i <= XART_QUAD_SEGS; i++) {
                    int32_t u = XART_QUAD_SEGS - i;
                    int32_t w0 = u * u, w1 = 2 * i * u, w2 = (int32_t)i * i;
                    int32_t den = XART_QUAD_SEGS * XART_QUAD_SEGS;
                    int32_t nx = w0 * p0x + w1 * cx + w2 * p1x;
                    int32_t ny = w0 * p0y + w1 * cy + w2 * p1y;
                    /* (n/den - s0) * sq, kept in one expression so the only
                     * rounding is the final shift. */
                    int32_t dx = (nx - den * sx0) * sq / den;
                    int32_t dy = (ny - den * sy0) * sq / den;
                    pts[np].x = (lv_coord_t)(ox + (int)((dx + (1 << (Q - 1))) >> Q));
                    pts[np].y = (lv_coord_t)(oy + (int)((dy + (1 << (Q - 1))) >> Q));
                    np++;
                }
            }
        }
        out[no].pts = &pts[start];
        out[no].n   = (uint16_t)(np - start);
        no++;
    }
    #undef MAPX
    #undef MAPY
    return no;
}
