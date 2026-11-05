/* radar_layout.c -- see radar_layout.h. */
#include "radar_layout.h"

#include <math.h>

static int overlap(int ax, int ay, int aw, int ah, const rl_rect_t *b)
{
    return ax < b->x + b->w && b->x < ax + aw &&
           ay < b->y + b->h && b->y < ay + ah;
}

/* Every corner of the rectangle inside the circle. A rectangle whose four
 * corners are inside a circle is wholly inside it. */
static int inside(const rl_scope_t *s, int x, int y, int w, int h)
{
    const int xs[2] = { x, x + w }, ys[2] = { y, y + h };
    long r2 = (long)s->r * s->r;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            long dx = xs[i] - s->cx, dy = ys[j] - s->cy;
            if (dx * dx + dy * dy > r2) return 0;
        }
    return 1;
}

void rl_spread_dots(const rl_scope_t *s, const float *ang, const int *rad,
                    int n, int min_sep, int *bx, int *by)
{
    if (n > RL_MAX) n = RL_MAX;
    const float step = 14.0f * (float)M_PI / 180.0f;
    long sep2 = (long)min_sep * min_sep;
    for (int i = 0; i < n; i++) {
        float a = ang[i];
        int x = 0, y = 0;
        /* 0, +step, -step, +2 step, ... up to a half turn either way. */
        for (int k = 0; k < 26; k++) {
            float t = a + (k & 1 ? 1.0f : -1.0f) * (float)((k + 1) / 2) * step;
            x = s->cx + (int)lroundf(sinf(t) * (float)rad[i]);
            y = s->cy - (int)lroundf(cosf(t) * (float)rad[i]);
            int clear = 1;
            for (int j = 0; j < i && clear; j++) {
                long dx = x - bx[j], dy = y - by[j];
                if (dx * dx + dy * dy < sep2) clear = 0;
            }
            /* Nor on a ring label: a dot over "100m" hides the scale. The
             * angle is arbitrary anyway -- only the radius means anything. */
            for (int k = 0; k < s->n_keep_out && clear; k++)
                if (overlap(x - RL_HALO, y - RL_HALO, 2 * RL_HALO, 2 * RL_HALO,
                            &s->keep_out[k])) clear = 0;
            if (clear) break;
            /* A ring this small has no room for another dot at any angle:
             * accept the overlap rather than spin; the label decides. */
        }
        bx[i] = x;
        by[i] = y;
    }
}

static int clear_at(const rl_scope_t *s, const int *bx, const int *by, int n,
                    const uint8_t *form, const rl_rect_t *out,
                    int x, int y, int w, int h)
{
    if (!inside(s, x, y, w, h)) return 0;
    for (int k = 0; k < s->n_keep_out; k++)
        if (overlap(x, y, w, h, &s->keep_out[k])) return 0;
    for (int j = 0; j < n; j++) {
        rl_rect_t halo = { bx[j] - RL_HALO, by[j] - RL_HALO,
                           2 * RL_HALO, 2 * RL_HALO };
        if (overlap(x, y, w, h, &halo)) return 0;
        if (form[j] != RL_HIDDEN && overlap(x, y, w, h, &out[j])) return 0;
    }
    return 1;
}

/* The spots around a dot, nearest first: beside it on the side facing the
 * centre (that side has room), the other side, under, over, the diagonals;
 * then the same again one step further out. */
static int try_form(const rl_scope_t *s, const int *bx, const int *by, int n,
                    const uint8_t *form, const rl_rect_t *out, int i,
                    int w, int h, rl_rect_t *got)
{
    int x0 = bx[i], y0 = by[i];
    int inward = x0 > s->cx;         /* right half: try the left side first */
    for (int g = 0; g < 2; g++) {
        int d = RL_HALO + 2 + g * 8;
        int cand[8][2] = {
            { inward ? x0 - d - w : x0 + d, y0 - h / 2 },
            { inward ? x0 + d : x0 - d - w, y0 - h / 2 },
            { x0 - w / 2, y0 + d },
            { x0 - w / 2, y0 - d - h },
            { x0 + d - 2, y0 + d - 2 },
            { x0 - d + 2 - w, y0 + d - 2 },
            { x0 + d - 2, y0 - d + 2 - h },
            { x0 - d + 2 - w, y0 - d + 2 - h },
        };
        for (int c = 0; c < 8; c++) {
            if (clear_at(s, bx, by, n, form, out, cand[c][0], cand[c][1], w, h)) {
                got->x = cand[c][0]; got->y = cand[c][1];
                got->w = w; got->h = h;
                return 1;
            }
        }
    }
    return 0;
}

/* One greedy pass, forms from [first] down. */
static int place_pass(const rl_scope_t *s, const int *bx, const int *by,
                      int n, const int *order,
                      const rl_size_t (*forms)[RL_FORMS], int first,
                      uint8_t *form, rl_rect_t *out)
{
    for (int i = 0; i < n; i++) form[i] = RL_HIDDEN;
    int placed = 0;
    for (int k = 0; k < n; k++) {
        int i = order ? order[k] : k;
        if (i < 0 || i >= n) continue;
        for (int f = first; f < RL_FORMS; f++) {
            rl_rect_t got;
            if (forms[i][f].w <= 0) continue;
            if (try_form(s, bx, by, n, form, out, i,
                         forms[i][f].w, forms[i][f].h, &got)) {
                out[i] = got;
                form[i] = (uint8_t)(RL_FULL + f);
                placed++;
                break;
            }
        }
    }
    return placed;
}

/* Greedy alone lets the first contacts take their long forms and crowd
 * out a later contact's short one. So run it three times -- every form,
 * then only the shorter two, then the short one alone -- and keep the
 * result that names the most contacts; on a tie the earlier, longer one. */
int rl_place_labels(const rl_scope_t *s, const int *bx, const int *by, int n,
                    const int *order, const rl_size_t (*forms)[RL_FORMS],
                    uint8_t *form, rl_rect_t *out)
{
    if (n > RL_MAX) n = RL_MAX;
    uint8_t tf[RL_MAX];
    rl_rect_t to[RL_MAX];
    int best = place_pass(s, bx, by, n, order, forms, 0, form, out);
    for (int first = 1; first < RL_FORMS && best < n; first++) {
        int got = place_pass(s, bx, by, n, order, forms, first, tf, to);
        if (got > best) {
            best = got;
            for (int i = 0; i < n; i++) { form[i] = tf[i]; out[i] = to[i]; }
        }
    }
    return best;
}

static int hexch(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

void rl_short_name(const char *name, char *out, int cap)
{
    if (cap <= 0) return;
    int len = 0;
    while (name[len]) len++;
    int mt = len == 10 && name[0] == 'M' && name[1] == 'T';
    for (int i = 2; mt && i < 10; i++)
        if (!hexch(name[i])) mt = 0;
    const char *src = mt ? name + 6 : name;
    int i = 0;
    for (; src[i] && i < cap - 1; i++) out[i] = src[i];
    out[i] = 0;
}
