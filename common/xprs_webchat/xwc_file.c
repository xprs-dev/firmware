/* xwc_file.c -- see the header. */
#include "xwc_file.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* SHA-256 a piece at a time: mbedTLS on the board, OpenSSL on the host. */
#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
static void *h_new(void)
{
    mbedtls_sha256_context *c = malloc(sizeof *c);
    if (c) { mbedtls_sha256_init(c); mbedtls_sha256_starts(c, 0); }
    return c;
}
static void h_add(void *c, const void *b, size_t n) { mbedtls_sha256_update(c, b, n); }
static void h_end(void *c, uint8_t out[32])
{
    mbedtls_sha256_finish(c, out);
    mbedtls_sha256_free(c);
    free(c);
}
#else
#include <openssl/evp.h>
static void *h_new(void)
{
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    if (c && EVP_DigestInit_ex(c, EVP_sha256(), NULL) != 1) { EVP_MD_CTX_free(c); c = NULL; }
    return c;
}
static void h_add(void *c, const void *b, size_t n) { EVP_DigestUpdate(c, b, n); }
static void h_end(void *c, uint8_t out[32])
{
    unsigned n = 32;
    EVP_DigestFinal_ex(c, out, &n);
    EVP_MD_CTX_free(c);
}
#endif

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

bool xwcf_parse(const char *ref, uint8_t sha[32], char ext[XWCF_EXT_MAX + 1])
{
    if (!ref) return false;
    const char *dot = strchr(ref, '.');
    if (!dot || strchr(dot + 1, '.')) return false;
    size_t hl = (size_t)(dot - ref), el = strlen(dot + 1);
    if (el < 1 || el > XWCF_EXT_MAX) return false;
    for (size_t i = 0; i < el; i++) {
        char c = dot[1 + i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    if (hl == 64) {
        for (int i = 0; i < 32; i++) {
            int a = hex_val(ref[2 * i]), b = hex_val(ref[2 * i + 1]);
            if (a < 0 || b < 0) return false;
            sha[i] = (uint8_t)(a << 4 | b);
        }
    } else if (hl == 43) {
        /* 43 characters are 258 bits: 32 bytes and two bits that must be 0. */
        uint32_t acc = 0;
        int bits = 0, n = 0;
        for (size_t i = 0; i < 43; i++) {
            int v = b64u_val(ref[i]);
            if (v < 0) return false;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (n < 32) sha[n++] = (uint8_t)(acc >> bits);
                acc &= (1u << bits) - 1;
            }
        }
        if (n != 32 || acc != 0) return false;
    } else {
        return false;
    }
    memcpy(ext, dot + 1, el + 1);
    return true;
}

void xwcf_hex(const uint8_t sha[32], char out[65])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hx[sha[i] >> 4];
        out[2 * i + 1] = hx[sha[i] & 15];
    }
    out[64] = 0;
}

const char *xwcf_type(const char *ext)
{
    static const struct { const char *e, *t; } map[] = {
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "png", "image/png" },
        { "gif", "image/gif" }, { "webp", "image/webp" }, { "avif", "image/avif" },
        { "bmp", "image/bmp" }, { "txt", "text/plain; charset=utf-8" },
        { "md", "text/markdown; charset=utf-8" }, { "pdf", "application/pdf" },
        { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" }, { "mp4", "video/mp4" },
        { "m4v", "video/mp4" }, { "mov", "video/quicktime" }, { "webm", "video/webm" },
        { "mkv", "video/x-matroska" }, { "ogv", "video/ogg" }, { "m4a", "audio/mp4" },
        { "wav", "audio/wav" }, { "heic", "image/heic" },
    };
    for (size_t i = 0; ext && i < sizeof map / sizeof map[0]; i++)
        if (strcmp(ext, map[i].e) == 0) return map[i].t;
    return "application/octet-stream";
}

/* ---- the store ---------------------------------------------------------- */
#define THUMBS "/.thumbnails"
#define OWN    "/own"

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static void mkdir_p(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) mkdir(dir, 0775);
}

/* Where a file may be, in the order a reader looks: the station's own
 * first (it never goes), then a stranger's. */
static const char *const k_places[4] = { OWN, OWN THUMBS, "", THUMBS };

bool xwcf_room(const xwcf_store_t *s, uint64_t need)
{
    char dirs[2][128];
    snprintf(dirs[0], sizeof dirs[0], "%s", s->dir);
    snprintf(dirs[1], sizeof dirs[1], "%s" THUMBS, s->dir);
    for (int round = 0; round < 64; round++) {
        uint64_t total = 0;
        time_t oldest_t = 0;
        char oldest[200] = "", path[200];
        /* A stranger's originals and previews share the budget; own/ is
         * neither counted nor touched. */
        for (int k = 0; k < 2; k++) {
            DIR *d = opendir(dirs[k]);
            if (!d) { if (k == 0) return false; continue; }
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strlen(e->d_name) != 64) continue;     /* a part, a folder, not ours */
                struct stat st;
                snprintf(path, sizeof path, "%.127s/%.64s", dirs[k], e->d_name);
                if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                total += (uint64_t)st.st_size;
                if (!oldest[0] || st.st_mtime < oldest_t) {
                    oldest_t = st.st_mtime;
                    snprintf(oldest, sizeof oldest, "%s", path);
                }
            }
            closedir(d);
        }
        uint64_t fre = 0, tot = 0;
        bool known = s->space && s->space(&fre, &tot);
        if (total + need <= s->budget && (!known || fre >= need + s->reserve)) return true;
        if (!oldest[0]) return false;
        if (remove(oldest) != 0) return false;
    }
    return false;
}

int xwcf_put_begin(const xwcf_store_t *s, xwcf_put_t *p, const uint8_t sha[32],
                   uint64_t length, bool staff, bool preview, const char **why)
{
    memset(p, 0, sizeof *p);
    memcpy(p->sha, sha, 32);
    if (length == 0) { *why = "no bytes"; return 400; }
    char hex[65], dir[140], other[200];
    xwcf_hex(sha, hex);
    mkdir_p(s->dir);
    bool held = false;
    for (int k = 0; k < 4 && !held; k++) {
        snprintf(other, sizeof other, "%s%s/%s", s->dir, k_places[k], hex);
        if (!exists(other)) continue;
        /* The station's own, held already: nothing to do but check the
         * bytes. A stranger's copy is the same bytes: staff keep it (pin). */
        if (k < 2 || !(staff && s->own)) held = true;
        else snprintf(p->pin, sizeof p->pin, "%s", other);
    }
    if (staff && s->own) {
        /* The station's own: as large as the volume, a reserve kept. */
        snprintf(dir, sizeof dir, "%s" OWN "%s", s->dir, preview ? THUMBS : "");
        if (!held && !p->pin[0]) {
            uint64_t fre = 0, tot = 0;
            if (!s->space || !s->space(&fre, &tot)) { *why = "cannot tell the room on the card"; return 503; }
            uint64_t keep = tot / 100 > XWCF_OWN_RESERVE_MIN ? tot / 100 : XWCF_OWN_RESERVE_MIN;
            if (length + keep > fre) { *why = "the card is full"; return 413; }
        }
        snprintf(other, sizeof other, "%s" OWN, s->dir);
        mkdir_p(other);
    } else {
        if (length > s->visitor_max) { *why = "larger than this station keeps"; return 413; }
        snprintf(dir, sizeof dir, "%s%s", s->dir, preview && s->own ? THUMBS : "");
        if (!held && !xwcf_room(s, length)) { *why = "no room for it here"; return 503; }
    }
    if (!held) {
        mkdir_p(dir);
        snprintf(p->path, sizeof p->path, "%.139s/%s", dir, hex);
    }
    p->h = h_new();
    if (!p->h) { *why = "no memory"; return 503; }
    /* Held, or pinned from a stranger's copy: the body is still read and
     * checked against the reference, and nothing written. */
    if (!held && !p->pin[0]) {
        snprintf(p->part, sizeof p->part, "%s/part.tmp", dir);
        p->f = fopen(p->part, "wb");
        if (!p->f) {
            uint8_t sink[32];
            h_end(p->h, sink);
            p->h = NULL;
            *why = "cannot write";
            return 503;
        }
    }
    return 0;
}

bool xwcf_put_add(xwcf_put_t *p, const void *buf, size_t n)
{
    if (!p->h) return false;
    h_add(p->h, buf, n);
    p->got += n;
    return !p->f || fwrite(buf, 1, n, (FILE *)p->f) == n;
}

int xwcf_put_end(xwcf_put_t *p, bool complete, const char **why)
{
    uint8_t got[32] = {0};
    if (p->h) h_end(p->h, got);
    p->h = NULL;
    bool wrote = p->f != NULL;
    if (p->f) fclose((FILE *)p->f);
    p->f = NULL;
    int code = 200;
    if (!complete) { *why = "short body"; code = 400; }
    else if (memcmp(got, p->sha, 32) != 0) { *why = "the bytes do not match the reference"; code = 400; }
    if (code != 200) {
        if (wrote) remove(p->part);
        return code;
    }
    if (wrote && rename(p->part, p->path) != 0) {
        remove(p->part);
        *why = "cannot write";
        return 503;
    }
    if (p->pin[0] && rename(p->pin, p->path) != 0) { *why = "cannot write"; return 503; }
    return 200;
}

void *xwcf_open(const xwcf_store_t *s, const uint8_t sha[32], uint64_t *size)
{
    char hex[65], path[200];
    xwcf_hex(sha, hex);
    for (int k = 0; k < 4; k++) {
        snprintf(path, sizeof path, "%s%s/%s", s->dir, k_places[k], hex);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        *size = (uint64_t)st.st_size;
        return f;
    }
    return NULL;
}

static bool digits(const char **c, uint64_t *v)
{
    if (**c < '0' || **c > '9') return false;
    uint64_t n = 0;
    while (**c >= '0' && **c <= '9') {
        if (n > (UINT64_MAX - 9) / 10) return false;
        n = n * 10 + (uint64_t)(*(*c)++ - '0');
    }
    *v = n;
    return true;
}

int xwcf_range(const char *hdr, uint64_t size, uint64_t *a, uint64_t *b)
{
    if (!hdr) return 0;
    while (*hdr == ' ') hdr++;
    if (strncmp(hdr, "bytes=", 6) != 0 || strchr(hdr, ',')) return 0;
    const char *c = hdr + 6;
    while (*c == ' ') c++;
    uint64_t x = 0, y = 0;
    if (*c == '-') {
        /* The last n bytes. */
        c++;
        if (!digits(&c, &y)) return 0;
        while (*c == ' ') c++;
        if (*c) return 0;
        if (y == 0 || size == 0) return -1;
        *a = y >= size ? 0 : size - y;
        *b = size - 1;
        return 1;
    }
    if (!digits(&c, &x) || *c++ != '-') return 0;
    bool open = !(*c >= '0' && *c <= '9');
    if (!open && !digits(&c, &y)) return 0;
    while (*c == ' ') c++;
    if (*c) return 0;
    if (!open && y < x) return 0;          /* not a valid range: the whole */
    if (x >= size) return -1;
    *a = x;
    *b = open || y >= size ? size - 1 : y;
    return 1;
}

/* ---- .acl: the rooms files were posted in ------------------------------ */
#define ACL_PREFIX 16
#define ACL_REC    (ACL_PREFIX + XWCF_ACL_CALL)
/* Records the table holds: a card's store keeps thousands of pictures, the
 * board's own flash a few dozen. Full, the records of files no longer held
 * go first; a table still full refuses the bind. */
#define ACL_MAX_CARD  16384
#define ACL_MAX_FLASH 512

static void acl_path(const xwcf_store_t *s, char *out, size_t cap)
{
    snprintf(out, cap, "%s/.acl", s->dir);
}

/* The record's index for [sha], or -1; [n] gets how many there are. */
static long acl_find(FILE *f, const uint8_t sha[32], long *n)
{
    uint8_t r[ACL_REC];
    long i = 0, at = -1;
    rewind(f);
    while (fread(r, 1, ACL_REC, f) == ACL_REC) {
        if (at < 0 && memcmp(r, sha, ACL_PREFIX) == 0) at = i;
        i++;
    }
    if (n) *n = i;
    return at;
}

/* Drop the records of files the store no longer holds. */
static long acl_prune(const xwcf_store_t *s, const char *path)
{
    char tmp[300];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) return -1;
    FILE *in = fopen(path, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    if (!out) { if (in) fclose(in); return -1; }
    uint8_t r[ACL_REC], sha[32];
    long kept = 0;
    while (fread(r, 1, ACL_REC, in) == ACL_REC) {
        /* Held under any digest with this prefix: the store names files by
         * the whole digest, so look for the prefix among the names. */
        memset(sha, 0, sizeof sha);
        memcpy(sha, r, ACL_PREFIX);
        char hex[65];
        xwcf_hex(sha, hex);
        hex[ACL_PREFIX * 2] = 0;
        bool held = false;
        for (int k = 0; k < 4 && !held; k++) {
            char dir[280];
            if (snprintf(dir, sizeof dir, "%s%s", s->dir, k_places[k]) >= (int)sizeof dir) continue;
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)))
                if (!strncmp(e->d_name, hex, ACL_PREFIX * 2)) { held = true; break; }
            closedir(d);
        }
        if (held && fwrite(r, 1, ACL_REC, out) == ACL_REC) kept++;
    }
    fclose(in);
    if (fclose(out) != 0) { remove(tmp); return -1; }
    remove(path);
    if (rename(tmp, path) != 0) return -1;
    return kept;
}

bool xwcf_acl_bind(const xwcf_store_t *s, const uint8_t sha[32], const char *room, bool only_new)
{
    if (!room || !room[0]) return false;
    char path[280];
    acl_path(s, path, sizeof path);
    uint8_t r[ACL_REC];
    memset(r, 0, sizeof r);
    memcpy(r, sha, ACL_PREFIX);
    strncpy((char *)r + ACL_PREFIX, room, XWCF_ACL_CALL - 1);
    long n = 0, at = -1;
    FILE *f = fopen(path, "r+b");
    if (f) {
        at = acl_find(f, sha, &n);
        if (at >= 0) {
            uint8_t old[ACL_REC];
            if (fseek(f, at * ACL_REC, SEEK_SET) != 0 || fread(old, 1, ACL_REC, f) != ACL_REC) { fclose(f); return false; }
            if (only_new || !memcmp(old, r, ACL_REC)) { fclose(f); return true; }
            bool ok = fseek(f, at * ACL_REC, SEEK_SET) == 0 && fwrite(r, 1, ACL_REC, f) == ACL_REC;
            return fclose(f) == 0 && ok;
        }
        fclose(f);
    }
    long max = s->own ? ACL_MAX_CARD : ACL_MAX_FLASH;
    if (n >= max && acl_prune(s, path) >= max) return false;
    f = fopen(path, "ab");
    if (!f) return false;
    bool ok = fwrite(r, 1, ACL_REC, f) == ACL_REC;
    return fclose(f) == 0 && ok;
}

bool xwcf_acl_room(const xwcf_store_t *s, const uint8_t sha[32], char room[XWCF_ACL_CALL])
{
    char path[280];
    acl_path(s, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    long at = acl_find(f, sha, NULL);
    bool ok = false;
    if (at >= 0) {
        uint8_t r[ACL_REC];
        ok = fseek(f, at * ACL_REC, SEEK_SET) == 0 && fread(r, 1, ACL_REC, f) == ACL_REC;
        if (ok) {
            memcpy(room, r + ACL_PREFIX, XWCF_ACL_CALL);
            room[XWCF_ACL_CALL - 1] = 0;
            ok = room[0] != 0;
        }
    }
    fclose(f);
    return ok;
}

static bool ref_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.';
}

int xwcf_refs(const char *wire, int len, void (*each)(const uint8_t sha[32], void *ctx), void *ctx)
{
    int found = 0;
    for (int i = 0; i + 5 <= len; i++) {
        if (memcmp(wire + i, "file:", 5) != 0) continue;
        if (i > 0 && ref_char(wire[i - 1])) continue;    /* `xfile:`, not a field */
        int a = i + 5, b = a;
        while (b < len && b - a < 80 && ref_char(wire[b])) b++;
        while (b > a && wire[b - 1] == '.') b--;          /* a sentence's full stop */
        char ref[81];
        memcpy(ref, wire + a, (size_t)(b - a));
        ref[b - a] = 0;
        uint8_t sha[32];
        char ext[XWCF_EXT_MAX + 1];
        if (xwcf_parse(ref, sha, ext)) { each(sha, ctx); found++; }
        i = b - 1;
    }
    return found;
}
