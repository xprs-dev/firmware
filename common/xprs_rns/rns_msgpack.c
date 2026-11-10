/* See rns_msgpack.h. */
#include "rns_msgpack.h"
#include <string.h>

static bool take(mp_rd_t *r, size_t n, const uint8_t **at)
{
    if (r->n - r->i < n) return false;
    *at = r->p + r->i;
    r->i += n;
    return true;
}

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int k = 0; k < n; k++) v = (v << 8) | p[k];
    return v;
}

bool mp_next(mp_rd_t *r, mp_obj_t *o)
{
    const uint8_t *h, *x;
    memset(o, 0, sizeof *o);
    if (!take(r, 1, &h)) return false;
    uint8_t c = *h;
    size_t len = 0;
    int lb = 0;                         /* length bytes of a str/bin/arr/map */
    if (c <= 0x7f) { o->type = MP_INT; o->i = c; return true; }
    if (c >= 0xe0) { o->type = MP_INT; o->i = (int8_t)c; return true; }
    if ((c & 0xf0) == 0x80) { o->type = MP_MAP; o->len = c & 0x0f; return true; }
    if ((c & 0xf0) == 0x90) { o->type = MP_ARR; o->len = c & 0x0f; return true; }
    if ((c & 0xe0) == 0xa0) { o->type = MP_STR; len = c & 0x1f; goto bytes; }
    switch (c) {
    case 0xc0: o->type = MP_NIL; return true;
    case 0xc2: case 0xc3: o->type = MP_BOOL; o->i = c == 0xc3; return true;
    case 0xc4: case 0xc5: case 0xc6:
        o->type = MP_BIN; lb = 1 << (c - 0xc4); break;
    case 0xd9: case 0xda: case 0xdb:
        o->type = MP_STR; lb = 1 << (c - 0xd9); break;
    case 0xca: {
        if (!take(r, 4, &x)) return false;
        uint32_t u = (uint32_t)be(x, 4);
        float f; memcpy(&f, &u, 4);
        o->type = MP_FLOAT; o->f = f; return true;
    }
    case 0xcb: {
        if (!take(r, 8, &x)) return false;
        uint64_t u = be(x, 8);
        double d; memcpy(&d, &u, 8);
        o->type = MP_FLOAT; o->f = d; return true;
    }
    case 0xcc: case 0xcd: case 0xce: case 0xcf: {
        int n = 1 << (c - 0xcc);
        if (!take(r, (size_t)n, &x)) return false;
        o->type = MP_INT; o->i = (int64_t)be(x, n); return true;
    }
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int n = 1 << (c - 0xd0);
        if (!take(r, (size_t)n, &x)) return false;
        uint64_t u = be(x, n);
        int64_t v = n == 8 ? (int64_t)u
                  : (int64_t)(u ^ (1ULL << (8 * n - 1))) - (int64_t)(1ULL << (8 * n - 1));
        o->type = MP_INT; o->i = v; return true;
    }
    case 0xdc: case 0xdd:
        if (!take(r, c == 0xdc ? 2 : 4, &x)) return false;
        o->type = MP_ARR; o->len = (size_t)be(x, c == 0xdc ? 2 : 4); return true;
    case 0xde: case 0xdf:
        if (!take(r, c == 0xde ? 2 : 4, &x)) return false;
        o->type = MP_MAP; o->len = (size_t)be(x, c == 0xde ? 2 : 4); return true;
    default:
        return false;                   /* ext types: not used by RNS here */
    }
    if (!take(r, (size_t)lb, &x)) return false;
    len = (size_t)be(x, lb);
bytes:
    if (!take(r, len, &o->s)) return false;
    o->len = len;
    return true;
}

bool mp_skip(mp_rd_t *r)
{
    mp_obj_t o;
    if (!mp_next(r, &o)) return false;
    if (o.type == MP_ARR) {
        for (size_t k = 0; k < o.len; k++) if (!mp_skip(r)) return false;
    } else if (o.type == MP_MAP) {
        for (size_t k = 0; k < 2 * o.len; k++) if (!mp_skip(r)) return false;
    }
    return true;
}

bool mp_map_find(const mp_rd_t *map, size_t n, const char *key, mp_rd_t *at)
{
    mp_rd_t r = *map;
    size_t kl = strlen(key);
    for (size_t k = 0; k < n; k++) {
        mp_obj_t o;
        if (!mp_next(&r, &o)) return false;
        bool hit = o.type == MP_STR && o.len == kl && memcmp(o.s, key, kl) == 0;
        if (!hit && (o.type == MP_ARR || o.type == MP_MAP)) {
            /* A non-string key with entries: skip them. */
            size_t m = o.type == MP_ARR ? o.len : 2 * o.len;
            for (size_t j = 0; j < m; j++) if (!mp_skip(&r)) return false;
        }
        if (hit) { *at = r; return true; }
        if (!mp_skip(&r)) return false;
    }
    return false;
}

/* ── writing ── */

static void put(mp_wr_t *w, const void *b, size_t n)
{
    if (w->err || w->cap - w->n < n) { w->err = true; return; }
    memcpy(w->b + w->n, b, n);
    w->n += n;
}
static void put1(mp_wr_t *w, uint8_t c) { put(w, &c, 1); }
static void put_be(mp_wr_t *w, uint64_t v, int n)
{
    uint8_t t[8];
    for (int k = 0; k < n; k++) t[k] = (uint8_t)(v >> (8 * (n - 1 - k)));
    put(w, t, (size_t)n);
}

void mp_nil(mp_wr_t *w) { put1(w, 0xc0); }
void mp_bool(mp_wr_t *w, bool v) { put1(w, v ? 0xc3 : 0xc2); }
void mp_uint(mp_wr_t *w, uint64_t v)
{
    if (v <= 0x7f) put1(w, (uint8_t)v);
    else if (v <= 0xff) { put1(w, 0xcc); put_be(w, v, 1); }
    else if (v <= 0xffff) { put1(w, 0xcd); put_be(w, v, 2); }
    else if (v <= 0xffffffffULL) { put1(w, 0xce); put_be(w, v, 4); }
    else { put1(w, 0xcf); put_be(w, v, 8); }
}
void mp_f64(mp_wr_t *w, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    put1(w, 0xcb);
    put_be(w, u, 8);
}
void mp_str(mp_wr_t *w, const char *s, size_t n)
{
    if (n < 32) put1(w, (uint8_t)(0xa0 | n));
    else if (n <= 0xff) { put1(w, 0xd9); put_be(w, n, 1); }
    else if (n <= 0xffff) { put1(w, 0xda); put_be(w, n, 2); }
    else { put1(w, 0xdb); put_be(w, n, 4); }
    put(w, s, n);
}
void mp_cstr(mp_wr_t *w, const char *s) { mp_str(w, s, strlen(s)); }
void mp_bin_head(mp_wr_t *w, size_t n)
{
    if (n <= 0xff) { put1(w, 0xc4); put_be(w, n, 1); }
    else if (n <= 0xffff) { put1(w, 0xc5); put_be(w, n, 2); }
    else { put1(w, 0xc6); put_be(w, n, 4); }
}
void mp_bin(mp_wr_t *w, const uint8_t *b, size_t n) { mp_bin_head(w, n); put(w, b, n); }
void mp_arr(mp_wr_t *w, size_t n)
{
    if (n <= 15) put1(w, (uint8_t)(0x90 | n));
    else if (n <= 0xffff) { put1(w, 0xdc); put_be(w, n, 2); }
    else { put1(w, 0xdd); put_be(w, n, 4); }
}
void mp_map(mp_wr_t *w, size_t n)
{
    if (n <= 15) put1(w, (uint8_t)(0x80 | n));
    else if (n <= 0xffff) { put1(w, 0xde); put_be(w, n, 2); }
    else { put1(w, 0xdf); put_be(w, n, 4); }
}
void mp_raw(mp_wr_t *w, const uint8_t *b, size_t n) { put(w, b, n); }
