/*
 * xapi_rns.c -- the visitor API over a Reticulum link (spec/API-HTTP.md "The
 * same API over a Reticulum link"): the two request paths the station serves
 * on links to its xprs.wapp destination (xprsrns_serve).
 *
 * /xprs/api carries one HTTP call, {m, p, q, b}. It is made, unchanged, to
 * this board's own httpd over the loopback: every handler answers it exactly
 * as it answers a browser, and nothing here knows what any of them do. The
 * call carries `X-XPRS-Via: rns`, which xapi_wc_local() reads (from
 * 127.0.0.1 only) as a visitor from the internet: a link is never the
 * station's own network. The answer is {s, t, b, h?}.
 *
 * /xprs/prove answers {session, identity}: a t:session signed by the station
 * whose m: is the link id and the client's nonce, and its t:identity, so a
 * client checks it reached the station it asked for whatever carried the
 * link (a gateway, a hub).
 *
 * Runs on the request task (xprsrns.c), which may block; never on httpd's.
 */
#include "xapi_rns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "rns_msgpack.h"
#include "xprsrns.h"
#include "xwc.h"

static const char *TAG = "xapi_rns";

/* What a call costs. A body is a list of chunks, never one block: on a C3
 * with BLE and the hotspot up the internal heap swings between 4 and 25 KB
 * free and its largest block can be under 2 KB, and while a handler runs (a
 * history reading the index) little is left. So a board without PSRAM
 * claims chunks before the call, down to HEAP_FLOOR, takes more one by one
 * while GROW_FLOOR stays free, and gives back the ones it did not fill once
 * the body is in; a body past that is answered 413. With PSRAM, chunks are
 * taken from it as the body arrives. */
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#define CHUNK      (32u * 1024u)
#define BODY_MAX   (2u * 1024u * 1024u)   /* the largest answer sent over a link */
#else
#define CHUNK      512u                   /* fits the holes a fragmented heap has */
/* A 20-row history is about 6.3 KB. The floor is what the handler, Wi-Fi's
 * receive buffers and BLE need meanwhile (14 KB left too few chunks for a
 * history; at 10 KB, with the uplink lock held for seconds, a history took the
 * heap under 4 KB, the handler stalled and the hub socket dropped). */
#define BODY_MAX   (8u * 1024u)
#define HEAP_FLOOR (12u * 1024u)
#define GROW_FLOOR (8u * 1024u)           /* past the claim, chunk by chunk */
#endif
#define CHUNKS     (BODY_MAX / CHUNK)
#define REQ_MAX    1024                   /* the request line and headers out */
#define RESP_HEAD  768                    /* the response's head in */
#define RX_CHUNK   512
#define QUERY_MAX  768
#define IO_TIMEOUT_S 30

/* ── a body: chunks of RAM ─────────────────────────────────────────────── */


typedef struct {
    uint8_t *c[CHUNKS];
    int      nc;                          /* chunks held (filled or claimed) */
    size_t   n;                           /* bytes in them */
    bool     claimed;                     /* claimed before the call: grow only to GROW_FLOOR */
    bool     over, err;
} body_t;

static void *chunk_alloc(void)
{
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    void *q = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (q) return q;
#endif
    return malloc(CHUNK);
}

#if !(defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
/* Claim chunks now, while there are holes for them; at least one. */
static bool body_claim(body_t *b)
{
    while (b->nc < (int)CHUNKS) {
        if (b->nc && heap_caps_get_free_size(MALLOC_CAP_8BIT) < HEAP_FLOOR + CHUNK) break;
        uint8_t *q = malloc(CHUNK);
        if (!q) break;
        b->c[b->nc++] = q;
    }
    b->claimed = true;
    return b->nc > 0;
}
#endif

static void body_add(body_t *b, const uint8_t *d, size_t n)
{
    if (b->over || b->err) return;
    while (n) {
        int i = (int)(b->n / CHUNK);
        if (i >= (int)CHUNKS) { b->over = true; return; }
        if (i >= b->nc) {
#ifdef GROW_FLOOR
            if (b->claimed && heap_caps_get_free_size(MALLOC_CAP_8BIT) < GROW_FLOOR + CHUNK) {
                b->over = true;
                return;
            }
#endif
            uint8_t *q = chunk_alloc();
            if (!q) {
                /* No RAM for the next chunk: past what this call can hold. */
                if (b->nc) b->over = true;
                else { ESP_LOGW(TAG, "body: no %u B in RAM", (unsigned)CHUNK); b->err = true; }
                return;
            }
            b->c[b->nc++] = q;
        }
        size_t at = b->n % CHUNK, k = CHUNK - at < n ? CHUNK - at : n;
        memcpy(b->c[i] + at, d, k);
        b->n += k; d += k; n -= k;
    }
}

/* The body is all here: the chunks it did not fill go back. */
static void body_done(body_t *b)
{
    int used = (int)((b->n + CHUNK - 1) / CHUNK);
    while (b->nc > used) { free(b->c[--b->nc]); b->c[b->nc] = NULL; }
}

static void body_free(body_t *b)
{
    while (b->nc) { free(b->c[--b->nc]); b->c[b->nc] = NULL; }
    b->n = 0;
}

static void body_read(const body_t *b, size_t off, uint8_t *buf, size_t n)
{
    while (n) {
        size_t at = off % CHUNK, k = CHUNK - at < n ? CHUNK - at : n;
        memcpy(buf, b->c[off / CHUNK] + at, k);
        off += k; buf += k; n -= k;
    }
}

/* ── the answer: msgpack head, the body, the tail (headers) ───────────── */

typedef struct {
    rns_src_t s;
    uint8_t *head; size_t hn;
    body_t body;
    uint8_t *tail; size_t tn;
} ans_t;

static bool ans_read(rns_src_t *s, size_t off, uint8_t *buf, size_t n)
{
    ans_t *a = (ans_t *)s;
    if (off > s->len || n > s->len - off) return false;
    while (n) {
        if (off < a->hn) {
            size_t k = a->hn - off < n ? a->hn - off : n;
            memcpy(buf, a->head + off, k);
            off += k; buf += k; n -= k;
        } else if (off < a->hn + a->body.n) {
            size_t bo = off - a->hn;
            size_t k = a->body.n - bo < n ? a->body.n - bo : n;
            body_read(&a->body, bo, buf, k);
            off += k; buf += k; n -= k;
        } else {
            size_t to = off - a->hn - a->body.n;
            size_t k = a->tn - to < n ? a->tn - to : n;
            memcpy(buf, a->tail + to, k);
            off += k; buf += k; n -= k;
        }
    }
    return true;
}

static void ans_release(rns_src_t *s)
{
    ans_t *a = (ans_t *)s;
    free(a->head);
    free(a->tail);
    body_free(&a->body);
    free(a);
}

/* {s, t, b, h?}: the body taken from [b] (its ownership passes). */
static rns_src_t *answer(int status, const char *type, body_t *b,
                         const uint8_t *hdrs, size_t hn, size_t nh)
{
    ans_t *a = calloc(1, sizeof *a);
    uint8_t *head = malloc(160);
    if (!a || !head) { free(a); free(head); body_free(b); return NULL; }
    mp_wr_t w;
    mp_wr_init(&w, head, 160);
    mp_map(&w, nh ? 4 : 3);
    mp_cstr(&w, "s"); mp_uint(&w, (uint64_t)status);
    mp_cstr(&w, "t"); mp_str(&w, type, strlen(type) < 100 ? strlen(type) : 100);
    mp_cstr(&w, "b"); mp_bin_head(&w, b->n);
    if (w.err) { free(a); free(head); body_free(b); return NULL; }
    a->head = head;
    a->hn = w.n;
    body_done(b);
    a->body = *b;
    memset(b, 0, sizeof *b);
    if (nh) {
        a->tail = malloc(hn + 8);
        if (a->tail) {
            mp_wr_t t;
            mp_wr_init(&t, a->tail, hn + 8);
            mp_cstr(&t, "h");
            mp_map(&t, nh);
            mp_raw(&t, hdrs, hn);
            a->tn = t.err ? 0 : t.n;
        }
        if (!a->tn) {                  /* no room for the headers: drop them */
            free(a->tail);
            a->tail = NULL;
            head[0] = 0x83;
        }
    }
    a->s.len = a->hn + a->body.n + a->tn;
    a->s.read = ans_read;
    a->s.release = ans_release;
    return &a->s;
}

static rns_src_t *answer_json(int status, const char *json)
{
    body_t b = {0};
    body_add(&b, (const uint8_t *)json, strlen(json));
    return answer(status, "application/json", &b, NULL, 0, 0);
}

/* ── the loopback call ──────────────────────────────────────────────────── */

/* The fixed buffers of a call, claimed once at start while the heap is still
 * whole: minutes after boot a C3's largest free block is under 1 KB, and a
 * station that claimed them per call would answer nothing. One task runs
 * every call (xprsrns.c), so one set serves them all; /xprs/prove borrows
 * the same buffers. */
typedef struct {
    char    head[REQ_MAX];                /* the request out, the head in */
    uint8_t buf[RX_CHUNK + 1];
    char    query[QUERY_MAX];
} work_t;
static work_t *s_w;


static bool send_all(int fd, const char *b, size_t n)
{
    while (n) {
        int w = send(fd, b, n, 0);
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

/* Percent-encoding for a query key or value. The handlers read the query
 * as it stands (httpd_query_key_value leaves %xx alone, and `kind=` wants
 * its commas literal), so only what would break the URL is encoded. */
static size_t qenc(char *out, size_t cap, const uint8_t *s, size_t n)
{
    static const char *hx = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        bool keep = isalnum(c) || strchr("-_.~,:/@!$'()*;", c);
        if (keep && c) { if (o + 1 >= cap) return 0; out[o++] = (char)c; }
        else { if (o + 3 >= cap) return 0; out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    out[o] = 0;
    return o;
}

/* A header the answer carries in `h`: what the API itself set, not what
 * framed or decorated the HTTP exchange. */
static bool header_kept(const char *name)
{
    static const char *const drop[] = {
        "content-type", "content-length", "transfer-encoding", "connection",
        "x-content-type-options", "date", "server", "keep-alive",
    };
    if (!strncasecmp(name, "access-control-", 15)) return false;
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++)
        if (!strcasecmp(name, drop[i])) return false;
    return true;
}

typedef struct {
    int status;
    char type[96];
    body_t body;
    uint8_t hdrs[256]; size_t hn, nh;
    bool chunked;
    long clen;                         /* -1: until the connection closes */
} resp_t;

/* Parse the head in [h] (NUL-terminated, no final blank line). */
static bool parse_head(char *h, resp_t *r)
{
    char *save = NULL;
    char *line = strtok_r(h, "\r\n", &save);
    if (!line || sscanf(line, "HTTP/1.%*d %d", &r->status) != 1) return false;
    snprintf(r->type, sizeof r->type, "application/octet-stream");
    r->clen = -1;
    mp_wr_t w;
    mp_wr_init(&w, r->hdrs, sizeof r->hdrs);
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "content-type")) snprintf(r->type, sizeof r->type, "%s", v);
        else if (!strcasecmp(line, "content-length")) r->clen = atol(v);
        else if (!strcasecmp(line, "transfer-encoding") && !strncasecmp(v, "chunked", 7)) r->chunked = true;
        if (header_kept(line)) {
            size_t before = w.n;
            mp_cstr(&w, line);
            mp_cstr(&w, v);
            if (w.err) { w.err = false; w.n = before; }       /* no room: left out */
            else r->nh++;
        }
    }
    r->hn = w.n;
    return true;
}

/* Read the body as framed: chunked, by length, or to the close. */
static bool read_body(int fd, resp_t *r, uint8_t *buf, size_t have)
{
    /* [buf] holds [have] bytes already read past the head; RX_CHUNK capacity. */
    if (!r->chunked) {
        long want = r->clen;
        size_t got = 0;
        if (have) { size_t k = want >= 0 && (long)have > want ? (size_t)want : have; body_add(&r->body, buf, k); got = k; }
        while (want < 0 || (long)got < want) {
            int n = recv(fd, buf, RX_CHUNK, 0);
            if (n <= 0) return want < 0;
            size_t k = want >= 0 && (long)(got + (size_t)n) > want ? (size_t)(want - (long)got) : (size_t)n;
            body_add(&r->body, buf, k);
            got += k;
            if (r->body.over) return true;
        }
        return true;
    }
    /* Chunked: a small state machine over whatever recv hands back. */
    enum { SIZE, DATA, CRLF, TRAILER } st = SIZE;
    char line[24];
    size_t ln = 0, left = 0;
    size_t pos = 0, n = have;
    for (;;) {
        while (pos < n) {
            uint8_t c = buf[pos];
            if (st == SIZE) {
                pos++;
                if (c == '\n') {
                    line[ln] = 0;
                    left = (size_t)strtoul(line, NULL, 16);
                    ln = 0;
                    st = left ? DATA : TRAILER;
                } else if (c != '\r' && ln < sizeof line - 1) line[ln++] = (char)c;
            } else if (st == DATA) {
                size_t k = n - pos < left ? n - pos : left;
                body_add(&r->body, buf + pos, k);
                pos += k; left -= k;
                if (!left) st = CRLF;
            } else if (st == CRLF) {
                pos++;
                if (c == '\n') st = SIZE;
            } else {                       /* TRAILER: up to the blank line */
                pos++;
                if (c == '\n') { if (ln == 0) return true; ln = 0; }
                else if (c != '\r') ln++;
            }
        }
        if (r->body.over) return true;
        int k = recv(fd, buf, RX_CHUNK, 0);
        if (k <= 0) return false;
        n = (size_t)k;
        pos = 0;
    }
}

static bool loopback(const char *method, const char *path, const char *query,
                     const uint8_t *body, size_t blen, resp_t *r)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(80) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    const char *why = "";
    char *head = s_w->head;
    uint8_t *buf = s_w->buf;
#if !(defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
    /* The body's chunks, claimed now: once the handler runs a board
     * without PSRAM has no 1 KB block left. */
    if (!body_claim(&r->body)) { why = "no room for the answer"; goto out; }
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { why = "connect"; goto out; }
    int hn = snprintf(head, REQ_MAX,
                      "%s %s%s%s HTTP/1.1\r\nHost: 127.0.0.1\r\nX-XPRS-Via: rns\r\n"
                      "Connection: close\r\nContent-Length: %u\r\n%s\r\n",
                      method, path, query[0] ? "?" : "", query, (unsigned)blen,
                      blen && body[0] == '{' ? "Content-Type: application/json\r\n" : "");
    if (hn <= 0 || hn >= REQ_MAX) { why = "request head too long"; goto out; }
    if (!send_all(fd, head, (size_t)hn) || (blen && !send_all(fd, (const char *)body, blen))) { why = "send"; goto out; }
    /* The head, then the body as it is framed. */
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        if (got >= RESP_HEAD - 1) { why = "response head too long"; goto out; }
        /* Never more at once than the body buffer takes: what follows the
         * head in the same read is handed to it whole. Reading up to the head
         * buffer's size let a short head bring more body than RX_CHUNK, and
         * every answer longer than that failed. */
        size_t want = RESP_HEAD - 1 - got;
        if (want > RX_CHUNK - 4) want = RX_CHUNK - 4;
        int n = recv(fd, head + got, want, 0);
        if (n <= 0) { why = "no response head"; goto out; }
        got += (size_t)n;
        head[got] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    size_t past = (size_t)(end + 4 - head);
    size_t extra = got - past;
    if (extra > RX_CHUNK) { why = "too much after the head"; goto out; }
    memcpy(buf, head + past, extra);
    *end = 0;
    if (!parse_head(head, r)) { why = "unreadable head"; goto out; }
    ok = read_body(fd, r, buf, extra);
    if (!ok) why = "body";
out:
    if (!ok) ESP_LOGW(TAG, "loopback %s %s failed at: %s (errno %d)", method, path, why, errno);
    close(fd);
    return ok;
}

/* ── /xprs/api ─────────────────────────────────────────────────────────── */

static bool str_of(const mp_obj_t *o, char *out, size_t cap)
{
    if (o->type == MP_STR || o->type == MP_BIN) {
        if (o->len >= cap) return false;
        memcpy(out, o->s, o->len);
        out[o->len] = 0;
        return true;
    }
    if (o->type == MP_INT) { snprintf(out, cap, "%lld", (long long)o->i); return true; }
    if (o->type == MP_BOOL) { snprintf(out, cap, "%s", o->i ? "true" : "false"); return true; }
    if (o->type == MP_FLOAT) { snprintf(out, cap, "%g", o->f); return true; }
    return false;
}

bool xapi_rns_api(const uint8_t *data, size_t len, const uint8_t link_id[16], rns_src_t **out)
{
    (void)link_id;
    mp_rd_t r, at;
    mp_obj_t o, v;
    mp_rd_init(&r, data, len);
    if (!mp_next(&r, &o) || o.type != MP_MAP) {
        *out = answer_json(400, "{\"ok\":false,\"error\":\"bad request\"}");
        return true;
    }
    size_t nmap = o.len;
    char method[12] = "GET", path[160] = "";
    if (mp_map_find(&r, nmap, "m", &at) && mp_next(&at, &v) && str_of(&v, method, sizeof method))
        for (char *c = method; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (!(mp_map_find(&r, nmap, "p", &at) && mp_next(&at, &v) && str_of(&v, path, sizeof path)) ||
        strncmp(path, "/api/", 5) != 0 || strpbrk(path, "?# \r\n")) {
        *out = answer_json(404, "{\"ok\":false,\"error\":\"not here\"}");
        return true;
    }
    if (strpbrk(method, " \r\n") || !method[0]) {
        *out = answer_json(400, "{\"ok\":false,\"error\":\"bad request\"}");
        return true;
    }
    /* The query: a string map, in the order it came. */
    if (!s_w) return false;
    char *query = s_w->query;
    memset(query, 0, QUERY_MAX);
    size_t qn = 0;
    if (mp_map_find(&r, nmap, "q", &at) && mp_next(&at, &v) && v.type == MP_MAP) {
        for (size_t k = 0; k < v.len; k++) {
            mp_obj_t key, val;
            char num[24];
            if (!mp_next(&at, &key) || key.type != MP_STR) break;
            if (!mp_next(&at, &val)) break;
            const uint8_t *vb = val.s;
            size_t vl = val.len;
            if (val.type == MP_INT || val.type == MP_BOOL || val.type == MP_FLOAT) {
                if (!str_of(&val, num, sizeof num)) continue;
                vb = (const uint8_t *)num;
                vl = strlen(num);
            } else if (val.type != MP_STR && val.type != MP_BIN) {
                if (val.type == MP_ARR || val.type == MP_MAP) break;
                continue;
            }
            size_t mark = qn;
            if (qn && qn + 1 < QUERY_MAX) query[qn++] = '&';
            size_t w = qenc(query + qn, QUERY_MAX - qn, key.s, key.len);
            if (!w && key.len) { qn = mark; query[qn] = 0; break; }
            qn += w;
            if (qn + 1 >= QUERY_MAX) { qn = mark; query[qn] = 0; break; }
            query[qn++] = '=';
            w = qenc(query + qn, QUERY_MAX - qn, vb, vl);
            if (!w && vl) { qn = mark; query[qn] = 0; break; }
            qn += w;
        }
    }
    const uint8_t *body = NULL;
    size_t blen = 0;
    if (mp_map_find(&r, nmap, "b", &at) && mp_next(&at, &v) && (v.type == MP_BIN || v.type == MP_STR)) {
        body = v.s;
        blen = v.len;
    }
    static resp_t rsv;
    resp_t *rs = &rsv;
    memset(rs, 0, sizeof *rs);
    bool ok = loopback(method, path, query, body, blen, rs);
    if (!ok || rs->body.err) {
        ESP_LOGW(TAG, "%s %s: the loopback call failed", method, path);
        body_free(&rs->body);
        *out = answer_json(502, "{\"ok\":false,\"error\":\"the station did not answer\"}");
        return true;
    }
    if (rs->body.over) {
        body_free(&rs->body);
        *out = answer_json(413, "{\"ok\":false,\"error\":\"too large over Reticulum\"}");
        return true;
    }
    ESP_LOGI(TAG, "%s %s -> %d, %u bytes", method, path, rs->status, (unsigned)rs->body.n);
    *out = answer(rs->status, rs->type, &rs->body, rs->hdrs, rs->hn, rs->nh);
    return true;
}

/* ── /xprs/prove ───────────────────────────────────────────────────────── */

bool xapi_rns_prove(const uint8_t *data, size_t len, const uint8_t link_id[16], rns_src_t **out)
{
    mp_rd_t r;
    mp_obj_t o;
    mp_rd_init(&r, data, len);
    *out = NULL;
    /* 8 to 64 of A-Za-z0-9_-; anything else is answered nil. */
    if (!mp_next(&r, &o) || (o.type != MP_STR && o.type != MP_BIN) || o.len < 8 || o.len > 64)
        return true;
    for (size_t i = 0; i < o.len; i++)
        if (!isalnum(o.s[i]) && o.s[i] != '_' && o.s[i] != '-') return true;
    char m[32 * 2 + 1 + 65];
    for (int i = 0; i < 16; i++) sprintf(m + 2 * i, "%02x", link_id[i]);
    m[32] = '.';
    memcpy(m + 33, o.s, o.len);
    m[33 + o.len] = 0;
    if (!s_w) return false;
    char *session = s_w->head, *identity = s_w->head + 400;
    uint8_t *b = (uint8_t *)s_w->query;
    int sn = xwc_station_session(m, session, 400);
    int in = xprsrns_hello(identity, 300);
    bool has_id = in > 0 && strstr(identity, " k:") && strstr(identity, " sig:");
    mp_wr_t w;
    mp_wr_init(&w, b, 760);
    mp_map(&w, 2);
    mp_cstr(&w, "session");
    if (sn > 0) mp_str(&w, session, (size_t)sn); else mp_nil(&w);
    mp_cstr(&w, "identity");
    if (has_id) mp_str(&w, identity, (size_t)in); else mp_nil(&w);
    if (w.err) return false;
    uint8_t *copy = malloc(w.n);       /* about 350 B: it fits a small hole */
    if (!copy) return false;
    memcpy(copy, b, w.n);
    *out = xprsrns_src_ram(copy, w.n);
    return *out != NULL;
}

void xapi_rns_start(void)
{
    if (!s_w) s_w = malloc(sizeof *s_w);
    if (!s_w) { ESP_LOGW(TAG, "no room for the call buffers: the API stays off Reticulum"); return; }
    /* Spill files an earlier build left behind mid-answer. */
    DIR *d = opendir("/idx");
    if (d) {
        struct dirent *e;
        char p[300];
        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "rnsb", 4)) {
                snprintf(p, sizeof p, "/idx/%s", e->d_name);
                remove(p);
            }
        closedir(d);
    }
    bool a = xprsrns_serve("/xprs/api", xapi_rns_api);
    bool p = xprsrns_serve("/xprs/prove", xapi_rns_prove);
    if (a && p) ESP_LOGI(TAG, "the visitor API answers over Reticulum links");
}
