/* See xprs_folder.h. Pure: no ESP-IDF, no allocation. */
#include "xprs_folder.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── base64url and hex ────────────────────────────────────────────────── */

static int b64u_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool xfolder_ref_sha(const char *ref, int len, uint8_t out[32])
{
    if (!ref || len <= 0) return false;
    int n = 0;
    while (n < len && ref[n] != '.') n++;
    if (n == 64) {
        for (int i = 0; i < 32; i++) {
            int h = hex_val(ref[2 * i]), l = hex_val(ref[2 * i + 1]);
            if (h < 0 || l < 0) return false;
            out[i] = (uint8_t)(h << 4 | l);
        }
        return true;
    }
    if (n != 43) return false;
    /* 43 characters: 14 full groups of 4 (42 chars -> 31.5 bytes) is not
     * how it falls; 43 = 10 groups of 4 (30 bytes) + 3 chars (2 bytes). */
    uint32_t acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < 43; i++) {
        int v = b64u_val(ref[i]);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < 32) out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o == 32;
}

uint32_t xfolder_qty_bytes(const char *v, int len)
{
    if (!v || len <= 0) return 0;
    uint64_t n = 0;
    int i = 0;
    while (i < len && v[i] >= '0' && v[i] <= '9') {
        n = n * 10 + (uint64_t)(v[i] - '0');
        if (n > 0xFFFFFFFFull) return 0;
        i++;
    }
    if (i == 0) return 0;
    while (i < len && v[i] == ' ') i++;
    uint64_t mul = 1;
    if (i < len) {
        char u = (char)tolower((unsigned char)v[i]);
        if (u == 'k') mul = 1024ull;
        else if (u == 'm') mul = 1024ull * 1024;
        else if (u == 'g') mul = 1024ull * 1024 * 1024;
        else if (u != 'b') return 0;
        if (mul != 1) i++;
        if (i < len && (v[i] == 'B' || v[i] == 'b')) i++;
        if (i != len) return 0;
    }
    n *= mul;
    return n > 0xFFFFFFFFull ? 0 : (uint32_t)n;
}

/* ── the listing ──────────────────────────────────────────────────────── */

static bool listing_line(const char *s, int len, xfolder_entry_t *e)
{
    int sp1 = 0;
    while (sp1 < len && s[sp1] != ' ') sp1++;
    if (sp1 == 0 || sp1 >= len) return false;
    int sp2 = sp1 + 1;
    while (sp2 < len && s[sp2] != ' ') sp2++;
    if (sp2 >= len) return false;
    if (!xfolder_ref_sha(s, sp1, e->sha)) return false;
    e->size = xfolder_qty_bytes(s + sp1 + 1, sp2 - sp1 - 1);
    const char *name = s + sp2 + 1;
    int nl = len - sp2 - 1;
    while (nl > 0 && (name[nl - 1] == ' ' || name[nl - 1] == '\r')) nl--;
    if (nl <= 0 || nl > XFOLDER_NAME_MAX) return false;
    memcpy(e->name, name, (size_t)nl);
    e->name[nl] = 0;
    return true;
}

int xfolder_listing_each(const char *text, size_t len,
                         bool (*cb)(const xfolder_entry_t *e, void *ctx),
                         void *ctx)
{
    if (!text || len < 4 || memcmp(text, "XFL1", 4) != 0) return -1;
    size_t i = 4;
    while (i < len && text[i] != '\n') i++;      /* the magic line */
    if (i < len) i++;
    int n = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        xfolder_entry_t ent;
        memset(&ent, 0, sizeof ent);
        if (listing_line(text + i, (int)(e - i), &ent)) {
            n++;
            if (cb && !cb(&ent, ctx)) return n;
        }
        i = e + 1;
    }
    return n;
}

/* ── descriptions ─────────────────────────────────────────────────────── */

static bool is_file_desc(const xprs_t *p)
{
    char t[8];
    xprs_type(p, t, sizeof t);
    return strcmp(t, "file") == 0;
}

bool xfolder_desc_folder(const xprs_t *p, uint8_t listing_sha[32])
{
    if (!is_file_desc(p)) return false;
    int kl = 0;
    const char *k = xprs_get(p, "kind", &kl);
    if (!k || kl != 6 || memcmp(k, "folder", 6) != 0) return false;
    int fl = 0;
    const char *f = xprs_get(p, "file", &fl);
    return f && xfolder_ref_sha(f, fl, listing_sha);
}

bool xfolder_desc_member(const xprs_t *p, xfolder_entry_t *out)
{
    if (!is_file_desc(p)) return false;
    int l = 0;
    if (xprs_get(p, "kind", &l) || xprs_get(p, "b", &l) || xprs_get(p, "n", &l))
        return false;
    const char *f = xprs_get(p, "file", &l);
    if (!f || !xfolder_ref_sha(f, l, out->sha)) return false;
    const char *s = xprs_get(p, "size", &l);
    out->size = s ? xfolder_qty_bytes(s, l) : 0;
    if (!out->size) return false;
    const char *n = xprs_get(p, "name", &l);
    if (!n || l <= 0 || l > XFOLDER_NAME_MAX) return false;
    memcpy(out->name, n, (size_t)l);
    out->name[l] = 0;
    return true;
}

/* ── firmware images ──────────────────────────────────────────────────── */

bool xfolder_image_of(const char *name, xfolder_image_t *out)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 8 || strcmp(name + n - 4, ".bin") != 0) return false;
    /* The version starts at the last '-' that is followed by a digit; a
     * board name may itself carry a dash (tdongle-s3, m5stack-core). */
    const char *dash = NULL;
    for (const char *c = name; *c; c++)
        if (*c == '-' && c[1] >= '0' && c[1] <= '9') dash = c;
    if (!dash || dash == name) return false;
    size_t bl = (size_t)(dash - name), vl = n - 4 - bl - 1;
    if (bl > XFOLDER_BOARD_MAX || vl == 0 || vl > XFOLDER_VER_MAX) return false;
    memcpy(out->board, name, bl);
    out->board[bl] = 0;
    memcpy(out->version, dash + 1, vl);
    out->version[vl] = 0;
    return true;
}

int xfolder_version_cmp(const char *a, const char *b)
{
    /* Numbers dot by dot; then a bare version outranks one with a -tag. */
    for (;;) {
        unsigned long x = strtoul(a, (char **)&a, 10);
        unsigned long y = strtoul(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        bool da = *a == '.', db = *b == '.';
        if (da && db) { a++; b++; continue; }
        if (da) { a++; if (strtoul(a, NULL, 10) || strchr(a, '.')) return 1; continue; }
        if (db) { b++; if (strtoul(b, NULL, 10) || strchr(b, '.')) return -1; continue; }
        break;
    }
    bool ta = *a == '-', tb = *b == '-';
    if (ta && !tb) return -1;
    if (!ta && tb) return 1;
    if (ta && tb) return strcmp(a + 1, b + 1);
    return 0;
}

bool xfolder_newer_for(const xfolder_entry_t *e, const char *board,
                       const char *running, xfolder_image_t *img)
{
    xfolder_image_t tmp;
    if (!img) img = &tmp;
    if (!xfolder_image_of(e->name, img)) return false;
    if (!board || strcmp(img->board, board) != 0) return false;
    if (!running || !running[0]) return true;
    return xfolder_version_cmp(img->version, running) > 0;
}
