/* Host test for radar_layout: the T-Deck home scope as measured on
 * 2026-10-24 (190 px scope, seven stations within five metres). */
#include "radar_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static int ov(const rl_rect_t *a, const rl_rect_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static int in_circle(const rl_scope_t *s, const rl_rect_t *r)
{
    int xs[2] = { r->x, r->x + r->w }, ys[2] = { r->y, r->y + r->h };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            long dx = xs[i] - s->cx, dy = ys[j] - s->cy;
            if (dx * dx + dy * dy > (long)s->r * s->r) return 0;
        }
    return 1;
}

int main(void)
{
    /* The scope: 190 px, labels inside r = 89, ring labels and the centre
     * flash kept clear. */
    const int size = 190, c = size / 2;
    rl_rect_t keep[3] = {
        { c + 21 - 8, c + 21 - 4, 24, 12 },    /* "10m"  */
        { c + 42 - 8, c + 42 - 4, 30, 12 },    /* "100m" */
        { c - 9, c - 9, 18, 18 },              /* centre flash */
    };
    rl_scope_t s = { c, c, 89, keep, 3 };

    /* Seven contacts, the case from the screenshot: all within ~5 m, so
     * every radius is small, and names ten characters long. */
    float ang[7]; int rad[7];
    const float deg[7] = { 300, 40, 150, 160, 200, 210, 205 };
    const int   rr[7]  = { 8, 8, 20, 28, 34, 34, 36 };
    for (int i = 0; i < 7; i++) {
        ang[i] = deg[i] * (float)M_PI / 180.0f;
        rad[i] = rr[i];
    }
    int bx[7], by[7];
    rl_spread_dots(&s, ang, rad, 7, 12, bx, by);

    /* Dots: distinct where the ring has room, radius kept. */
    for (int i = 0; i < 7; i++) {
        long dx = bx[i] - c, dy = by[i] - c;
        int got = (int)lround(sqrt((double)(dx * dx + dy * dy)));
        CHECK(abs(got - rad[i]) <= 1, "dot %d radius %d, wanted %d", i, got, rad[i]);
    }
    /* Contacts 4, 5 and 6 sat within 10 degrees of each other on the same
     * ring -- the pile in the screenshot. They are spread now. */
    for (int i = 4; i < 7; i++)
        for (int j = 4; j < i; j++) {
            long dx = bx[i] - bx[j], dy = by[i] - by[j];
            CHECK(dx * dx + dy * dy >= 12 * 12, "dots %d and %d still touch", i, j);
        }

    rl_size_t fm[7][RL_FORMS];
    int order[7];
    for (int i = 0; i < 7; i++) {
        fm[i][0] = (rl_size_t){ 64, 26 };   /* "MT5F458B3F" over "~2m" */
        fm[i][1] = (rl_size_t){ 64, 13 };   /* the name alone */
        fm[i][2] = (rl_size_t){ 28, 13 };   /* "8B3F" */
        order[i] = i;                       /* nearest first */
    }
    uint8_t form[7]; rl_rect_t out[7];
    int placed = rl_place_labels(&s, bx, by, 7, order, fm, form, out);

    CHECK(placed >= 3, "only %d names placed", placed);
    for (int i = 0; i < 7; i++) {
        if (form[i] == RL_HIDDEN) continue;
        CHECK(in_circle(&s, &out[i]), "name %d crosses the rim", i);
        for (int k = 0; k < 3; k++)
            CHECK(!ov(&out[i], &keep[k]), "name %d covers keep-out %d", i, k);
        for (int j = 0; j < 7; j++) {
            rl_rect_t halo = { bx[j] - RL_HALO, by[j] - RL_HALO, 16, 16 };
            CHECK(!ov(&out[i], &halo), "name %d covers dot %d", i, j);
            if (j != i && form[j] != RL_HIDDEN)
                CHECK(!ov(&out[i], &out[j]), "names %d and %d overlap", i, j);
        }
    }
    /* The nearest contact is the one a person looks for: it gets a name. */
    CHECK(form[0] != RL_HIDDEN, "the nearest contact lost its name");

    /* A lone contact near the rim on the left keeps its name inside the
     * circle -- the MTD8F3D33F case, which ran past the rim. */
    {
        float a1 = 290.0f * (float)M_PI / 180.0f; int r1 = 70;
        int x1, y1; rl_spread_dots(&s, &a1, &r1, 1, 12, &x1, &y1);
        rl_size_t f[1][RL_FORMS] = { { { 70, 26 }, { 70, 13 }, { 28, 13 } } };
        int o1 = 0; uint8_t f1; rl_rect_t r;
        rl_place_labels(&s, &x1, &y1, 1, &o1, f, &f1, &r);
        CHECK(f1 != RL_HIDDEN, "a lone contact lost its name");
        CHECK(f1 == RL_HIDDEN || in_circle(&s, &r), "the lone name crosses the rim");
    }

    /* Twelve contacts on one tiny ring: some names must go, none may overlap. */
    {
        float a[12]; int rd[12], x[12], y[12], o[12];
        rl_size_t fs[12][RL_FORMS];
        for (int i = 0; i < 12; i++) {
            a[i] = (float)i * 0.1f; rd[i] = 8; o[i] = i;
            fs[i][0] = (rl_size_t){ 64, 26 };
            fs[i][1] = (rl_size_t){ 64, 13 };
            fs[i][2] = (rl_size_t){ 28, 13 };
        }
        rl_spread_dots(&s, a, rd, 12, 12, x, y);
        uint8_t f[12]; rl_rect_t r[12];
        rl_place_labels(&s, x, y, 12, o, fs, f, r);
        for (int i = 0; i < 12; i++)
            for (int j = 0; j < i; j++)
                if (f[i] && f[j]) CHECK(!ov(&r[i], &r[j]), "crowd: %d over %d", i, j);
    }

    /* The bench case of 2026-10-24: six stations, all at 5 m, so all on one
     * ring of a 156 px scope, names 10 characters. The first layout named
     * two of them; with the short form there is room for most. */
    {
        const int sz = 156, cc = sz / 2, R = sz / 2 - 4;
        rl_rect_t kp[3] = {
            { cc + (int)(R * 33 / 100 * 0.707f) - 12, cc + (int)(R * 33 / 100 * 0.707f) - 8, 28, 20 },
            { cc + (int)(R * 66 / 100 * 0.707f) - 12, cc + (int)(R * 66 / 100 * 0.707f) - 8, 34, 20 },
            { cc - 9, cc - 9, 18, 18 },
        };
        rl_scope_t sc = { cc, cc, R - 3, kp, 3 };
        const char *nm[6] = { "MT66B7F06F", "MT4211CF29", "X3SQS6",
                              "X1LC7H", "MT859DCE94", "MT0CA9CDCF" };
        float a[6]; int rd[6], x[6], y[6], o[6];
        rl_size_t fs[6][RL_FORMS];
        for (int i = 0; i < 6; i++) {
            unsigned hh = 5381;
            for (const char *p = nm[i]; *p; p++) hh = hh * 33 + (unsigned char)*p;
            a[i] = (float)(hh % 360) * (float)M_PI / 180.0f;
            rd[i] = i == 1 ? R * 45 / 100 : 28;
            o[i] = i;
            char sh[12]; rl_short_name(nm[i], sh, sizeof sh);
            int L = (int)strlen(nm[i]), Ls = (int)strlen(sh);
            fs[i][0] = (rl_size_t){ L * 7 + 1, 24 };
            fs[i][1] = (rl_size_t){ L * 7 + 1, 12 };
            fs[i][2] = (rl_size_t){ Ls * 7 + 1, 12 };
        }
        rl_spread_dots(&sc, a, rd, 6, 12, x, y);
        uint8_t f[6]; rl_rect_t r[6];
        int got = rl_place_labels(&sc, x, y, 6, o, fs, f, r);
        CHECK(got >= 5, "bench case: only %d of 6 named", got);
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 2; k++) {
                rl_rect_t halo = { x[i] - RL_HALO, y[i] - RL_HALO, 16, 16 };
                CHECK(!ov(&halo, &kp[k]), "bench: dot %d covers ring label %d", i, k);
            }
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < i; j++)
                if (f[i] && f[j]) CHECK(!ov(&r[i], &r[j]), "bench: %d over %d", i, j);
    }

    /* The short name is Meshtastic's own: the last four hex digits. */
    {
        char b[12];
        rl_short_name("MT66B7F06F", b, sizeof b);
        CHECK(!strcmp(b, "F06F"), "short of MT66B7F06F is %s", b);
        rl_short_name("X3SQS6", b, sizeof b);
        CHECK(!strcmp(b, "X3SQS6"), "a callsign stays whole, got %s", b);
        rl_short_name("MTXYZ", b, sizeof b);
        CHECK(!strcmp(b, "MTXYZ"), "not a node id: stays whole, got %s", b);
    }

    printf("radar_layout: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
