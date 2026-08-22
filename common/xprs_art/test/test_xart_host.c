/* Host test for the mark's geometry: the fit, the pen and the flattening.
 * A wrong scale looks plausible on a screenshot; here it is a number. */
#include <stdio.h>
#include <string.h>
#include "xprs_art.h"

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { \
    g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

int main(void)
{
    printf("xprs_art host tests\n");
    lv_point_t pts[XART_PTS_TRIAD];
    xart_poly_t poly[XART_POLY_TRIAD];
    int pen = 0;

    /* The T-Deck's splash box. */
    int n = xart_build(XART_G_TRIAD, 0, 6, 320, 166,
                       pts, XART_PTS_TRIAD, poly, XART_POLY_TRIAD, &pen);
    CHECK(n == 10, "triad polylines = %d, want 10", n);

    int total = 0;
    for (int i = 0; i < n; i++) total += poly[i].n;
    CHECK(total == XART_PTS_TRIAD, "triad points = %d, want %d",
          total, XART_PTS_TRIAD);

    /* Every point inside the box it was given, pen included. */
    int x0 = 9999, y0 = 9999, x1 = -9999, y1 = -9999;
    for (int i = 0; i < total; i++) {
        if (pts[i].x < x0) x0 = pts[i].x;
        if (pts[i].y < y0) y0 = pts[i].y;
        if (pts[i].x > x1) x1 = pts[i].x;
        if (pts[i].y > y1) y1 = pts[i].y;
    }
    CHECK(x0 - pen >= 0 && x1 + pen <= 320, "x span %d..%d pen %d escapes 0..320", x0, x1, pen);
    CHECK(y0 - pen >= 6 && y1 + pen <= 6 + 166, "y span %d..%d pen %d escapes the box", y0, y1, pen);

    /* Aspect preserved: the source is 252x236, so w/h must match within a
     * pixel of rounding. */
    int mw = x1 - x0, mh = y1 - y0;
    CHECK(mw * 236 / 252 >= mh - 2 && mw * 236 / 252 <= mh + 2,
          "aspect off: %dx%d (source 252x236)", mw, mh);

    /* Centred horizontally. */
    int left = x0, right = 320 - x1;
    CHECK(left - right <= 2 && right - left <= 2,
          "not centred: %d left, %d right", left, right);

    /* The star's four rays must share a centre: the vertical's midpoint and
     * the horizontal's midpoint are the same point. */
    lv_point_t sp[XART_PTS_STAR];
    xart_poly_t spoly[XART_POLY_STAR];
    int spen = 0;
    int sn = xart_build(XART_G_STAR, 0, 0, 68, 68,
                        sp, XART_PTS_STAR, spoly, XART_POLY_STAR, &spen);
    CHECK(sn == 4, "star polylines = %d, want 4", sn);
    int vcx = (spoly[0].pts[0].x + spoly[0].pts[1].x) / 2;
    int vcy = (spoly[0].pts[0].y + spoly[0].pts[1].y) / 2;
    int hcx = (spoly[1].pts[0].x + spoly[1].pts[1].x) / 2;
    int hcy = (spoly[1].pts[0].y + spoly[1].pts[1].y) / 2;
    CHECK(vcx == hcx && vcy == hcy, "star centre split: (%d,%d) vs (%d,%d)",
          vcx, vcy, hcx, hcy);
    /* And the star is square, so its box is too. */
    CHECK(spoly[0].pts[1].y - spoly[0].pts[0].y ==
          spoly[1].pts[1].x - spoly[1].pts[0].x, "star not square");

    /* The pen reproduces the SVG: a 120-unit icon box is stroke-width 9. */
    lv_point_t bp[XART_PTS_STAR];
    xart_poly_t bpoly[XART_POLY_STAR];
    int bpen = 0;
    xart_build(XART_G_STAR, 0, 0, 120, 120, bp, XART_PTS_STAR,
               bpoly, XART_POLY_STAR, &bpen);
    CHECK(bpen >= 8 && bpen <= 10, "120-box pen = %d, the SVG says 9", bpen);

    /* Waves must be monotonic in x and actually curve in y -- a flattening
     * bug that emits the control points verbatim would still be "smooth". */
    const xart_poly_t *w = &poly[7];
    CHECK(w->n == 13, "wave points = %d, want 13", w->n);
    int rising = 0, falling = 0;
    for (int i = 1; i < w->n; i++) {
        CHECK(w->pts[i].x >= w->pts[i - 1].x, "wave x went backwards at %d", i);
        if (w->pts[i].y < w->pts[i - 1].y) rising++;
        if (w->pts[i].y > w->pts[i - 1].y) falling++;
    }
    CHECK(rising > 1 && falling > 1, "wave does not undulate (%d up, %d down)",
          rising, falling);

    /* Capacity refusal, rather than a buffer overrun. */
    CHECK(xart_build(XART_G_TRIAD, 0, 0, 320, 166, pts, 10, poly,
                     XART_POLY_TRIAD, &pen) == -1, "short pts_cap not refused");
    CHECK(xart_build(XART_G_TRIAD, 0, 0, 320, 166, pts, XART_PTS_TRIAD, poly,
                     3, &pen) == -1, "short out_cap not refused");

    printf("%d checks, %d failed\n", g_checks, g_fail);
    printf("triad %dx%d at (%d,%d) pen %d | star pen %d\n",
           mw, mh, x0, y0, pen, spen);
    return g_fail ? 1 : 0;
}
