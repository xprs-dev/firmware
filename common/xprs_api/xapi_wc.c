/* xapi_wc.c -- see the header. */
#include "xapi_wc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lwip/opt.h"   /* LWIP_IPV6: see xapi_wc_local */
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "bech32.h"
#include "esp_http_client.h"
#include "nostr_keys.h"
#include "xapi_send.h"
#include "xprs.h"
#include "xprsindex.h"
#include "xwc.h"
#include "xwc_file.h"
#include "xprs_core.h"
#include "xprs_config.h"

static const char *TAG = "xapi_wc";
static const xprs_api_cfg_t *s_cfg;

/* A buffer of its own: an admin or rooms answer with members listed does
 * not fit the API's shared 2 KB, and a cut-off JSON answer is worse than
 * none. Claimed once at start. */
#define WC_BUF 4096
static char *s_buf;

/* ---- the engine's state: key=value lines on the station's storage ----
 * xcfg keeps a fixed table in NVS, so the engine's records (the
 * administrator, the settings, the rooms and their keys) live in a small
 * file of their own, read once and written whole on a change. */
#define KV_PATH   "/idx/wcstate.txt"
#define KV_MAX    24
#define KV_KEY    24
#define KV_VAL    1600
static struct { char key[KV_KEY]; char *val; } s_kv[KV_MAX];
static int s_nkv;

static void kv_load(void)
{
    FILE *f = fopen(KV_PATH, "r");
    if (!f) return;
    char *line = malloc(KV_VAL + KV_KEY + 2);
    if (!line) { fclose(f); return; }
    while (fgets(line, KV_VAL + KV_KEY + 2, f) && s_nkv < KV_MAX) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
        /* Record separators travel as \x1e / \x1f, which a line keeps. */
        snprintf(s_kv[s_nkv].key, KV_KEY, "%s", line);
        s_kv[s_nkv].val = strdup(v);
        if (s_kv[s_nkv].val) s_nkv++;
    }
    free(line);
    fclose(f);
}

static bool kv_save(void)
{
    FILE *f = fopen(KV_PATH ".new", "w");
    if (!f) return false;
    for (int i = 0; i < s_nkv; i++)
        if (s_kv[i].val && s_kv[i].val[0]) fprintf(f, "%s=%s\n", s_kv[i].key, s_kv[i].val);
    fclose(f);
    remove(KV_PATH);
    return rename(KV_PATH ".new", KV_PATH) == 0;
}

static const char *kv_get(const char *key)
{
    for (int i = 0; i < s_nkv; i++)
        if (strcmp(s_kv[i].key, key) == 0) return s_kv[i].val;
    return NULL;
}

static bool kv_set(const char *key, const char *val)
{
    for (int i = 0; i < s_nkv; i++) {
        if (strcmp(s_kv[i].key, key) != 0) continue;
        char *nv = strdup(val ? val : "");
        if (!nv) return false;
        free(s_kv[i].val);
        s_kv[i].val = nv;
        return kv_save();
    }
    if (s_nkv >= KV_MAX) return false;
    snprintf(s_kv[s_nkv].key, KV_KEY, "%s", key);
    s_kv[s_nkv].val = strdup(val ? val : "");
    if (!s_kv[s_nkv].val) return false;
    s_nkv++;
    return kv_save();
}

/* ---- the other callbacks ---------------------------------------------- */
static uint32_t now_cb(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;
}

static uint32_t uptime_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static const uint8_t *peer_key_cb(const char *call)
{
    return s_cfg && s_cfg->peer_key ? s_cfg->peer_key(call) : NULL;
}

static void random_cb(uint8_t *buf, size_t n) { esp_fill_random(buf, n); }

static bool air_cb(const char *wire, int len)
{
    return s_cfg && s_cfg->send_wire && s_cfg->send_wire(wire, len, NULL, NULL, 0);
}

/* A hosted room's wire into the archive, and what came of it: kept, kept
 * already, or not kept now (a full write queue, no archive yet), which the
 * gate answers 503 rather than claim it filed. */
static int file_cb(const char *wire, int len)
{
    if (!s_cfg || !s_cfg->index) return XWC_FILE_BUSY;
    switch (xprsindex_add3(s_cfg->index, wire, len, 0, false, now_cb(), XI_B_LAN)) {
    case XI_ADD_STORED:    return XWC_FILED;
    case XI_ADD_DUPLICATE: return XWC_FILE_HELD;
    default:               return XWC_FILE_BUSY;
    }
}

/* ---- the peer's network --------------------------------------------- */
bool xapi_wc_local(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&sa, &sl) != 0) return false;
    uint32_t ip = 0;
    if (sa.ss_family == AF_INET) {
        ip = ntohl(((struct sockaddr_in *)&sa)->sin_addr.s_addr);
#if LWIP_IPV6
    /* httpd listens on a v6 socket when lwIP has IPv6; a v4 peer then
     * arrives v4-mapped. Without IPv6 (the T-Dongle) the socket is plain
     * v4 and this branch has no type to compile against. */
    } else if (sa.ss_family == AF_INET6) {
        const uint8_t *a = ((struct sockaddr_in6 *)&sa)->sin6_addr.s6_addr;
        static const uint8_t v4map[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
        if (memcmp(a, v4map, 12) == 0) {
            ip = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) | ((uint32_t)a[14] << 8) | a[15];
        } else {
            if ((a[0] & 0xfe) == 0xfc) return true;          /* fc00::/7 */
            if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true; /* fe80::/10 */
            static const uint8_t lo[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
            return memcmp(a, lo, 16) == 0 && httpd_req_get_hdr_value_len(req, "X-XPRS-Via") <= 0;
        }
#endif
    } else return false;
    uint8_t o0 = ip >> 24, o1 = (ip >> 16) & 255;
    /* A call the station makes to itself for a visitor on a Reticulum link
     * (xapi_rns.c) says so: that visitor is on the internet, never on the
     * station's network. Read from the loopback only; any other peer's
     * header means nothing. */
    if (o0 == 127 && httpd_req_get_hdr_value_len(req, "X-XPRS-Via") > 0) return false;
    return o0 == 127 || o0 == 10 || (o0 == 172 && o1 >= 16 && o1 < 32) ||
           (o0 == 192 && o1 == 168) || (o0 == 169 && o1 == 254) ||
           (o0 == 100 && o1 >= 64 && o1 < 128);
}

/* The bytes this answer's body may have, 0 when nothing limits it. A call
 * the station makes to itself for a visitor on a Reticulum link (xapi_rns.c)
 * says how much it can carry; like X-XPRS-Via it is read from the loopback
 * only. A handler that lists things sends as many as fit and no more. */
size_t xapi_wc_room(httpd_req_t *req)
{
    char v[16];
    if (httpd_req_get_hdr_value_str(req, "X-XPRS-Room", v, sizeof v) != ESP_OK) return 0;
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage sa;
    socklen_t sl = sizeof sa;
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&sa, &sl) != 0) return 0;
    bool lo = false;
    if (sa.ss_family == AF_INET) {
        lo = (ntohl(((struct sockaddr_in *)&sa)->sin_addr.s_addr) >> 24) == 127;
#if LWIP_IPV6
    } else if (sa.ss_family == AF_INET6) {
        const uint8_t *a = ((struct sockaddr_in6 *)&sa)->sin6_addr.s6_addr;
        static const uint8_t v4lo[13] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,127 };
        static const uint8_t lo6[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        lo = memcmp(a, v4lo, 13) == 0 || memcmp(a, lo6, 16) == 0;
#endif
    }
    return lo ? (size_t)strtoul(v, NULL, 10) : 0;
}

/* ---- the request's pieces ------------------------------------------- */
static void query_val(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[256] = {0};
    out[0] = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK)
        httpd_query_key_value(q, key, out, cap);
}

/* The body, into [buf]; false on a short read. */
static bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = 0;
    while (total < req->content_len && total < (int)cap - 1) {
        int r = httpd_req_recv(req, buf + total, cap - 1 - (size_t)total);
        if (r <= 0) return false;
        total += r;
    }
    buf[total] = 0;
    return true;
}

static const char *status_line(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

static esp_err_t answer(httpd_req_t *req, int code, const char *json)
{
    xapi_resp_json(req);
    httpd_resp_set_status(req, status_line(code));
    return httpd_resp_send(req, json, (ssize_t)strlen(json));
}

/* ---- /api/session --------------------------------------------------- */
static esp_err_t h_session_get(httpd_req_t *req)
{
    char nonce[XWC_NONCE_LEN];
    xwc_nonce(nonce);
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":true,\"nonce\":\"%s\"}", nonce);
    return answer(req, 200, out);
}

/* The wire out of a body that is JSON {"wire":...}, `wire=` or the packet. */
static int wire_of(char *body, char *wire, size_t cap)
{
    const char *w = body;
    if (strstr(body, "\"wire\"")) {
        const char *j = strstr(body, "\"wire\"");
        j = strchr(j + 6, '"');
        if (!j) return 0;
        j++;
        size_t o = 0;
        while (*j && *j != '"' && o < cap - 1) {
            if (*j == '\\' && j[1]) j++;
            wire[o++] = *j++;
        }
        wire[o] = 0;
        w = wire;
    } else if (strncmp(body, "wire=", 5) == 0) {
        w = body + 5;
    }
    if (w != wire) snprintf(wire, cap, "%s", w);
    int n = (int)strlen(wire);
    while (n > 0 && (wire[n - 1] == '\n' || wire[n - 1] == '\r' || wire[n - 1] == ' ')) wire[--n] = 0;
    return n;
}

static esp_err_t h_session_post(httpd_req_t *req)
{
    char *body = s_buf, *wire = s_buf + 700;
    if (!read_body(req, body, 700)) return xapi_resp_error(req, "400 Bad Request", "short body");
    int n = wire_of(body, wire, 300);
    char token[XWC_TOKEN_LEN], call[XWC_CALL_LEN], why[64];
    int code = xwc_prove(wire, n, token, call, why, sizeof why);
    ESP_LOGI(TAG, "session: %s -> %d %s (up %lus)", call, code, code == 200 ? token : why,
             (unsigned long)uptime_cb());
    if (code != 200) return xapi_resp_error(req, status_line(code), why);
    /* A proven key is a hearing: the station's devices, hears: and gossip
     * learn the visitor is here even when they only came back to read. */
    if (s_cfg && s_cfg->visitor_heard) s_cfg->visitor_heard(call);
    char out[384];
    int on = snprintf(out, sizeof out, "{\"ok\":true,\"session\":\"%s\",\"call\":\"%s\"", token, call);
    if (s_cfg && s_cfg->session_extra && on > 0 && on < (int)sizeof out - 2)
        on += s_cfg->session_extra(call, out + on, sizeof out - (size_t)on - 2);
    if (on < 0 || on > (int)sizeof out - 2) on = (int)strlen(out);
    out[on++] = '}';
    out[on] = 0;
    return answer(req, 200, out);
}

/* ---- /api/admin ----------------------------------------------------- */
static esp_err_t h_admin_get(httpd_req_t *req)
{
    char token[XWC_TOKEN_LEN + 8];
    query_val(req, "session", token, sizeof token);
    xapi_wc_public_poll();
    int code = xwc_admin_json(s_buf, WC_BUF, token, xapi_wc_local(req));
    return answer(req, code, s_buf);
}

static esp_err_t h_admin_post(httpd_req_t *req)
{
    char *body = s_buf + 2048;
    if (!read_body(req, body, WC_BUF - 2048)) return xapi_resp_error(req, "400 Bad Request", "short body");
    int code = xwc_admin_post(body, xapi_wc_local(req), s_buf, 2048);
    ESP_LOGI(TAG, "admin: %.80s -> %d %.60s (up %lus)", body, code, s_buf, (unsigned long)uptime_cb());
    return answer(req, code, s_buf);
}

/* ---- /api/rooms ----------------------------------------------------- */
/* One `t:blog` row of the room into the index's fold (xwc_topics_row). */
static bool topics_emit(const xprsidx_rec_t *rec, void *arg)
{
    if (strcasecmp(rec->to, (const char *)arg) == 0)
        xwc_topics_row(rec->wire, rec->len <= XPRSIDX_WIRE_MAX ? rec->len : XPRSIDX_WIRE_MAX, rec->ts);
    return true;
}

/* ?topics=<call>: a forum's topics, a gallery's events (spec/API-HTTP.md),
 * folded from the room's newest posts: a window, never the whole room. */
static esp_err_t rooms_topics(httpd_req_t *req, const char *call, const char *token)
{
    int code = xwc_topics_begin(call, token, s_buf, WC_BUF);
    if (code != 200) return answer(req, code, s_buf);
    const char *room = xwc_room_of(call);
    if (s_cfg && s_cfg->index && room) {
        /* The room was judged readable above; its rows are the room's
         * mail, so the index's section 36 rule is satisfied here. */
        xprsidx_query_t q = { .type = XI_T_BLOG, .only = room, .newest_first = true,
                              .limit = XWC_TOPICS_WINDOW, .trusted = true };
        xprsindex_pause_writes(s_cfg->index, true);
        xprsindex_query(s_cfg->index, &q, topics_emit, (void *)room);
        xprsindex_pause_writes(s_cfg->index, false);
    }
    /* A hundred topics outgrow the shared buffer on a board with PSRAM;
     * without the room, the newest that fit. */
    size_t cap = (size_t)XWC_TOPICS_MAX * 160 + 64;
    char *out = cap > WC_BUF ? malloc(cap) : NULL;
    if (!out) { out = s_buf; cap = WC_BUF; }
    code = xwc_topics_end(out, cap);
    esp_err_t err = answer(req, code, out);
    if (out != s_buf) free(out);
    return err;
}

static esp_err_t h_rooms_get(httpd_req_t *req)
{
    char token[XWC_TOKEN_LEN + 8], topics[XWC_CALL_LEN + 4];
    query_val(req, "session", token, sizeof token);
    query_val(req, "topics", topics, sizeof topics);
    if (topics[0]) return rooms_topics(req, topics, token);
    int code = xwc_rooms_json(s_buf, WC_BUF, token);
    return answer(req, code, s_buf);
}

static esp_err_t h_rooms_post(httpd_req_t *req)
{
    char *body = s_buf + 2560;
    if (!read_body(req, body, WC_BUF - 2560)) return xapi_resp_error(req, "400 Bad Request", "short body");
    int code = xwc_rooms_post(body, s_buf, 2560);
    return answer(req, code, s_buf);
}

/* ---- /api/xprs/file/<ref>: files by reference (XPRS.md 7.7) -----------
 * The station keeps what a visitor stores under its digest and hands it to
 * whoever asks for it by reference (spec/API-HTTP.md). Generic: an image in
 * a post is one user, nothing here knows it. On a card when the board has
 * one, else a small corner of the board's own storage, which the archive,
 * the log and the engine's state need first: a file there is small, the
 * whole store is capped, and the oldest file goes to make room. Bytes are
 * streamed through the shared buffer and hashed on the way; a file is never
 * held whole in RAM. Everything here is public: the board keeps no private
 * file, so there is nothing for a 403 to protect.
 *
 * With a memory card (on /sdcard, or the archive's own card on /idx) the
 * store has a card's room, a picture's preview (`?preview=1`, XPRS.md
 * 7.7.7) is kept in `.thumbnails/` beside the originals, and what the
 * station's staff store is its own, in `own/`: photographs and films of any
 * size the card holds, never evicted. Where each goes is xwc_file.c's
 * (xwcf_put_*, xwcf_room, xwcf_open), host-tested; this is the socket. */
#define FILE_DIR_SD      "/sdcard/xfiles"
#define FILE_DIR_IDX     "/idx/xfiles"
#ifdef XPRS_QEMU
/* The emulated C3 has no card. Its flash volume (832 KB, the archive and
 * the log on it too) stands in for one, so the collections can be driven
 * over HTTP (tools/qemu_collections_test.py); a stranger's files sized to
 * it. Never on a real board: there a card is a card. */
#define FILE_MAX_SD      (64u * 1024u)
#define FILE_BUDGET_SD   (128u * 1024u)
#else
#define FILE_MAX_SD      (2u * 1024u * 1024u)
#define FILE_BUDGET_SD   (32u * 1024u * 1024u)
#endif
#define FILE_MAX_IDX     (96u * 1024u)
#define FILE_BUDGET_IDX  (192u * 1024u)
#define FILE_RESERVE     (128u * 1024u)   /* left free on the volume, always */
#define FILE_PUTS_MIN    12               /* stores a minute, all visitors */

static bool s_on_sd(void)
{
    struct stat st;
    return stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode);
}
/* A card, wherever it is mounted: room for photographs. */
static bool s_card(void)
{
#ifdef XPRS_QEMU
    return true;
#endif
    return s_on_sd() || (s_cfg && s_cfg->on_card && s_cfg->on_card());
}
static const char *file_dir(void) { return s_on_sd() ? FILE_DIR_SD : FILE_DIR_IDX; }
static uint32_t file_max(void) { return s_card() ? FILE_MAX_SD : FILE_MAX_IDX; }

/* The volume the store is on. */
static bool file_space(uint64_t *fre, uint64_t *tot)
{
    return esp_vfs_fat_info(s_on_sd() ? "/sdcard" : "/idx", tot, fre) == ESP_OK;
}

/* The store as it stands now: a card may come or go between two asks. */
static void file_store(xwcf_store_t *st)
{
    *st = (xwcf_store_t){
        .dir = file_dir(), .visitor_max = file_max(),
        .budget = s_card() ? FILE_BUDGET_SD : FILE_BUDGET_IDX,
        .reserve = FILE_RESERVE, .own = s_card(), .space = file_space,
        /* The flash volume's FAT allocates 4 KB at a time. */
        .cluster = s_on_sd() ? 0 : 4096u,
    };
}

/* The reference at the end of the request's path, or false. */
static bool file_ref(httpd_req_t *req, char *ref, size_t cap, uint8_t sha[32], char ext[XWCF_EXT_MAX + 1])
{
    const char *p = req->uri + strlen("/api/xprs/file/");
    size_t n = strcspn(p, "?#");
    if (n == 0 || n >= cap) return false;
    memcpy(ref, p, n);
    ref[n] = 0;
    return xwcf_parse(ref, sha, ext);
}

/* Everything of [n] bytes onto the socket, or false: a reader that takes
 * nothing for the server's send timeout (five seconds) is gone. */
static bool send_all(httpd_req_t *req, const char *b, size_t n)
{
    while (n) {
        int w = httpd_send(req, b, n);
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

/* ---- long file transfers, off the httpd worker ------------------------
 * The API server has one worker task; a film streamed from it holds every
 * other visitor (the page, the chat, the map) for the minutes it takes. A
 * file answer longer than XFER_INLINE is handed to a task of its own with
 * httpd's async request copy, and the worker goes back to the others.
 *
 * Only answers, not uploads. In the pinned IDF (5.2.1) the worker still
 * selects on a socket it lent out, and after the handler returns it drains
 * the request's unread body itself (httpd_req_delete): an upload handed
 * over would be read by two tasks at once. A POST stays on the worker,
 * streamed as before; a GET's request has no body, and the client says
 * nothing while it is being answered.
 *
 * What the worker still does on a lent socket is notice the client leave
 * (a player that seeks closes the old connection and opens another): it
 * closes the session, and lwIP hands the same descriptor number to the
 * next connection. So the transfer marks the session (its ctx, freed by
 * httpd when the session closes), and stops the moment it is gone instead
 * of writing the rest of a film into somebody else's answer. */
#define XFER_INLINE   (64u * 1024u)   /* longer answers go to the task */
#define XFER_DEPTH    2               /* transfers waiting; past that, 503 */
#define XFER_CHUNK    4096
/* The loop is fread (FatFs and the card driver beneath it: the SPI or
 * SDMMC transaction and the FAT sector walk, about 1.5 KB at their
 * deepest), httpd_send into lwIP's tcp_write (under 1 KB), a log line on
 * the way out (vprintf, about 1 KB) and this frame (under 200 bytes; the
 * chunk buffer and the head live in the heap). 4 KB leaves a margin over
 * that sum; the high-water mark is logged after the first transfer so a
 * board on a bench says what it really took. */
#define XFER_STACK    4096

typedef struct {
    uint32_t magic;
    volatile bool busy;      /* a transfer is writing to this session */
    volatile bool gone;      /* httpd closed the session */
} xfer_mark_t;
#define XFER_MAGIC 0x58464552u   /* "XFER" */
static portMUX_TYPE s_xmux = portMUX_INITIALIZER_UNLOCKED;

/* httpd closes a session: a transfer still on it stops at its next write. */
static void xfer_mark_free(void *p)
{
    xfer_mark_t *m = p;
    bool free_it;
    taskENTER_CRITICAL(&s_xmux);
    m->gone = true;
    free_it = !m->busy;
    taskEXIT_CRITICAL(&s_xmux);
    if (free_it) free(m);
}

typedef struct {
    httpd_req_t *req;         /* the async copy: owns the socket until complete */
    xfer_mark_t *mark;
    FILE        *f;
    uint64_t     at, len;
    int          head_len;
    char         head[512];
} xfer_job_t;

static QueueHandle_t s_xq;
static char *s_xbuf;

static bool xfer_send(xfer_job_t *j, const char *b, size_t n)
{
    while (n) {
        if (j->mark->gone) return false;
        int w = httpd_send(j->req, b, n);
        if (w <= 0) return false;
        b += w;
        n -= (size_t)w;
    }
    return true;
}

static void xfer_task(void *arg)
{
    (void)arg;
    bool measured = false;
    for (;;) {
        xfer_job_t *j;
        if (xQueueReceive(s_xq, &j, portMAX_DELAY) != pdTRUE) continue;
        bool ok = xfer_send(j, j->head, (size_t)j->head_len);
        if (ok && (j->at > 0x7fffffffu || fseek(j->f, (long)j->at, SEEK_SET) != 0)) ok = false;
        uint64_t len = ok ? j->len : 0;
        while (len) {
            size_t want = len < XFER_CHUNK ? (size_t)len : XFER_CHUNK;
            size_t r = fread(s_xbuf, 1, want, j->f);
            if (r == 0 || !xfer_send(j, s_xbuf, r)) { ok = false; break; }
            len -= r;
        }
        fclose(j->f);
        bool gone;
        taskENTER_CRITICAL(&s_xmux);
        gone = j->mark->gone;
        j->mark->busy = false;
        taskEXIT_CRITICAL(&s_xmux);
        if (gone) free(j->mark);
        /* A reader that stopped reading: close it, as a failed handler
         * would. Never once httpd closed it: the number may be another's. */
        else if (!ok) httpd_sess_trigger_close(j->req->handle, httpd_req_to_sockfd(j->req));
        httpd_req_async_handler_complete(j->req);
        free(j);
        if (!measured) {
            measured = true;
            ESP_LOGI(TAG, "file transfer task: %u bytes of its %u-byte stack never used",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)XFER_STACK);
        }
    }
}

/* Claimed at start, while the heap is whole: a task stack must be
 * internal memory, and on a board without PSRAM a late one may not fit. */
static void xfer_start(void)
{
    if (s_xq) return;
#if !(defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
    /* A board without PSRAM has no card-sized files to stream (96 KB on its
     * own flash) and no 8 KB to keep for a task and its buffer that would
     * idle: with BLE and the hotspot up the C3's heap is at its floor. Its
     * long answers, if a card is ever fitted, are served by the worker. */
    if (!s_card()) return;
#endif
    s_xbuf = malloc(XFER_CHUNK);
    s_xq = s_xbuf ? xQueueCreate(XFER_DEPTH, sizeof(xfer_job_t *)) : NULL;
    if (!s_xq || xTaskCreatePinnedToCore(xfer_task, "xfer", XFER_STACK, NULL, 5, NULL,
                                         XPRS_WORK_CORE) != pdPASS) {
        ESP_LOGW(TAG, "no file transfer task: long files are served by the worker");
        if (s_xq) vQueueDelete(s_xq);
        free(s_xbuf);
        s_xq = NULL;
        s_xbuf = NULL;
    }
}

/* Hand [f] (open, owned from here) and the head to the transfer task.
 *   1 handed over: the worker returns at once
 *   0 not possible here (no task, no memory): serve it inline
 *  -1 the queue is full: 503 answered, [f] closed */
static int xfer_offer(httpd_req_t *req, FILE *f, const char *head, int head_len,
                      uint64_t at, uint64_t len)
{
    if (!s_xq) return 0;
    /* The session's mark: ours from an earlier transfer on this keep-alive
     * connection, or a new one; a session carrying somebody else's ctx is
     * served inline. */
    xfer_mark_t *m = req->sess_ctx;
    if (m && (req->free_ctx != xfer_mark_free || m->magic != XFER_MAGIC)) return 0;
    if (uxQueueSpacesAvailable(s_xq) == 0) {
        fclose(f);
        httpd_resp_set_hdr(req, "Retry-After", "2");
        answer(req, 503, "{\"ok\":false,\"error\":\"busy, try again\"}");
        return -1;
    }
    xfer_job_t *j = calloc(1, sizeof *j);
    if (!j) return 0;
    if (!m) {
        m = calloc(1, sizeof *m);
        if (!m) { free(j); return 0; }
        m->magic = XFER_MAGIC;
        req->sess_ctx = m;
        req->free_ctx = xfer_mark_free;
    }
    if (httpd_req_async_handler_begin(req, &j->req) != ESP_OK) { free(j); return 0; }
    m->busy = true;
    j->mark = m;
    j->f = f;
    j->at = at;
    j->len = len;
    j->head_len = head_len < (int)sizeof j->head ? head_len : (int)sizeof j->head - 1;
    memcpy(j->head, head, (size_t)j->head_len);
    if (xQueueSend(s_xq, &j, 0) != pdTRUE) {
        /* Filled between the look and the send: only this task sends, so
         * this does not happen; undone all the same. */
        m->busy = false;
        httpd_req_async_handler_complete(j->req);
        free(j);
        return 0;
    }
    return 1;
}

/* ---- who a file is served to (XPRS.md 11.2) ----------------------------
 * A file posted in a hosted room is that room's: served to whoever may read
 * it (xwc_may_read_room: anyone for a public room, its members and the
 * administrator for a private one), decided at each ask, so a room made
 * private later closes its old files too. The page proves the visitor with
 * `?session=`. A file posted nowhere is public, as before. */
static void bind_one(const uint8_t sha[32], void *room)
{
    xwcf_store_t st;
    file_store(&st);
    if (!xwcf_acl_bind(&st, sha, room, false)) ESP_LOGW(TAG, "file store: a file of %s left unbound", (const char *)room);
}
static void bind_cb(const char *wire, int len, const char *room)
{
    xwcf_refs(wire, len, bind_one, (void *)room);
}

/* GET (and HEAD) /api/xprs/file/<ref>: the whole file, or the part a
 * `Range:` asks for (206), so a film plays from where the viewer seeks to.
 * The answer is written by hand: httpd's own sends a body it holds whole
 * (with its Content-Length) or chunks without one, and a player wants the
 * length of a part it never holds. Streamed from the file a chunk at a time;
 * a long one by the transfer task (above), a short one here. */
static esp_err_t h_file_get(httpd_req_t *req)
{
    char ref[80], ext[XWCF_EXT_MAX + 1], hex[65];
    uint8_t sha[32];
    if (!file_ref(req, ref, sizeof ref, sha, ext)) return answer(req, 400, "{\"ok\":false,\"error\":\"not a file reference\"}");
    xwcf_store_t st;
    file_store(&st);
    uint64_t size = 0, a = 0, b = 0;
    FILE *f = xwcf_open(&st, sha, &size);
    if (!f) return answer(req, 404, "{\"ok\":false,\"error\":\"not held\"}");
    char room[XWCF_ACL_CALL], token[XWC_TOKEN_LEN + 8];
    bool roomed = xwcf_acl_room(&st, sha, room);
    if (roomed) {
        query_val(req, "session", token, sizeof token);
        if (!xwc_may_read_room(room, token[0] ? token : NULL)) {
            fclose(f);
            return answer(req, 403, "{\"ok\":false,\"error\":\"not a public file\"}");
        }
    }
    xwcf_hex(sha, hex);
    char range[64] = "";
    if (httpd_req_get_hdr_value_len(req, "Range") > 0)
        httpd_req_get_hdr_value_str(req, "Range", range, sizeof range);
    int part = xwcf_range(range[0] ? range : NULL, size, &a, &b);
    if (part == 0) { a = 0; b = size ? size - 1 : 0; }
    uint64_t len = part < 0 || size == 0 ? 0 : b - a + 1;
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\nAccept-Ranges: bytes\r\n"
        "Cache-Control: %s, max-age=31536000, immutable\r\nETag: \"%s\"\r\n"
        "X-Content-Type-Options: nosniff\r\n%s",
        part > 0 ? "206 Partial Content" : part < 0 ? "416 Range Not Satisfiable" : "200 OK",
        xwcf_type(ext), (unsigned long long)len, roomed ? "private" : "public", hex,
        roomed ? "" : "Access-Control-Allow-Origin: *\r\n");
    if (part > 0)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Range: bytes %llu-%llu/%llu\r\n",
                      (unsigned long long)a, (unsigned long long)b, (unsigned long long)size);
    else if (part < 0)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Range: bytes */%llu\r\n",
                      (unsigned long long)size);
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (len > XFER_INLINE && req->method != HTTP_HEAD) {
        int handed = xfer_offer(req, f, head, n, a, len);
        if (handed) return ESP_OK;      /* on its way, or 503 answered */
    }
    bool ok = send_all(req, head, (size_t)n);
    /* FAT and newlib's long seek to 2 GB: past that a part cannot be
     * reached, and the connection is closed rather than lied to. */
    if (ok && len && req->method != HTTP_HEAD) {
        if (a > 0x7fffffffu || fseek(f, (long)a, SEEK_SET) != 0) len = 0, ok = false;
        while (len) {
            size_t want = len < WC_BUF ? (size_t)len : WC_BUF;
            size_t r = fread(s_buf, 1, want, f);
            if (r == 0 || !send_all(req, s_buf, r)) { ok = false; break; }
            len -= r;
        }
    }
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

/* POST /api/xprs/file/<ref>[?preview=1][&session=<token>]: the body is
 * streamed to the store as it arrives and hashed on the way (xwcf_put_*);
 * a file is never held in RAM, whatever its size. A session that proves
 * the station's staff (xwc_is_uploader) stores the station's own: on a
 * card, no cap but the card's free space, and never evicted. On the worker,
 * however long: see the transfer task's note on why an upload cannot be
 * handed over in this IDF. */
static esp_err_t h_file_post(httpd_req_t *req)
{
    char ref[80], ext[XWCF_EXT_MAX + 1], token[XWC_TOKEN_LEN + 8], pv[4];
    uint8_t sha[32];
    /* A long-range station is a repeater and archiver: it hosts no files
     * (docs/profiles.md). */
    if (xcfg_long_range())
        return answer(req, 403, "{\"ok\":false,\"error\":\"this station hosts no files (long-range profile)\"}");
    if (!file_ref(req, ref, sizeof ref, sha, ext)) return answer(req, 400, "{\"ok\":false,\"error\":\"not a file reference\"}");
    query_val(req, "session", token, sizeof token);
    query_val(req, "preview", pv, sizeof pv);
    /* `room=`: the upload is for a hosted room's post, bound before the post
     * so a private picture is never open in between. Only for somebody who
     * reads that room; never re-binding a file already posted. */
    char roomq[XWC_CALL_LEN + 4];
    query_val(req, "room", roomq, sizeof roomq);
    const char *room = roomq[0] ? xwc_room_of(roomq) : NULL;
    if (roomq[0] && (!room || !xwc_may_read_room(room, token[0] ? token : NULL)))
        return answer(req, 403, "{\"ok\":false,\"error\":\"not a room you read\"}");
    bool staff = token[0] && xwc_is_uploader(token);
    /* One visitor's limit is the core's; a board has one HTTP worker and a
     * small flash, so it caps every visitor together. The staff are not
     * counted: an event's pictures arrive in a burst. */
    if (!staff) {
        static int64_t s_win;
        static int s_puts;
        int64_t now = esp_timer_get_time() / 1000000;
        if (now - s_win >= 60) { s_win = now; s_puts = 0; }
        if (++s_puts > FILE_PUTS_MIN) return answer(req, 429, "{\"ok\":false,\"error\":\"slow down\"}");
    }
    xwcf_store_t st;
    file_store(&st);
    static xwcf_put_t put;      /* 800 bytes of paths: off the httpd stack, one worker */
    const char *why = "";
    char out[200];
    int code = xwcf_put_begin(&st, &put, sha, req->content_len, staff, pv[0] == '1', &why);
    if (code) {
        snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"%s\"}", why);
        return answer(req, code, out);
    }
    size_t total = 0;
    bool ok = true, wrote = true;
    int idle = 0;
    while (total < req->content_len) {
        size_t want = req->content_len - total;
        int r = httpd_req_recv(req, s_buf, want < WC_BUF ? want : WC_BUF);
        /* A slow phone on the hotspot pauses; three silent waits (ninety
         * seconds) and it has gone. */
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++idle < 3) continue;
        if (r <= 0) { ok = false; break; }
        idle = 0;
        if (!xwcf_put_add(&put, s_buf, (size_t)r)) { ok = wrote = false; break; }
        total += (size_t)r;
    }
    code = xwcf_put_end(&put, ok, &why);
    if (!wrote) { code = 503; why = "cannot write"; }
    if (code != 200) {
        snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"%s\"}", why);
        return answer(req, code, out);
    }
    if (room && !xwcf_acl_bind(&st, sha, room, true))
        return answer(req, 507, "{\"ok\":false,\"error\":\"too many room files held\"}");
    if (staff) ESP_LOGI(TAG, "file store: %s kept as the station's own, %u bytes", ref, (unsigned)total);
    snprintf(out, sizeof out, "{\"ok\":true,\"ref\":\"%s\",\"size\":%u}", ref, (unsigned)total);
    return answer(req, 200, out);
}

/* ---- the hooks ------------------------------------------------------ */
/* The owners' phones, asked where they are: the board's to do (radio). */
static int locate_cb(void)
{
    return s_cfg && s_cfg->locate ? s_cfg->locate() : 0;
}

int xapi_wc_services(char *buf, size_t cap)
{
    /* Every panel the board serves: the page hides what is not listed. A
     * long-range station (docs/profiles.md) keeps the chat and what it
     * repeats and archives, and offers no gallery, music or library. */
    bool lr = xcfg_long_range();
    int n = xwc_services_json(buf, cap, "chat,traffic,devices,stats,station,settings,alerts",
                              s_card() && !lr);
    /* Files by reference (/api/xprs/file/<ref>), as large as this board
     * keeps; a long-range station still serves what it holds, takes none. */
    if (n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n, ",\"file\":{\"get\":true,\"put\":%s,\"max\":%u}",
                      lr ? "false" : "true", (unsigned)file_max());
    return n;
}

/* ---- the public rooms' moderators (XPRS.md 10.10.3) ------------------
 * The list is published at xprs.dev and read from there only: no key is in
 * the build. It is asked for when the station's WiFi gets an address (the
 * event that says the site may be in reach), at most every twelve hours of
 * uptime, and after an ask that failed ten minutes later, then twenty, up
 * to twelve hours; the last list read is kept
 * in the engine's state and stands while the site is out of reach.
 *
 * The engine is not locked, and its callers so far are on the httpd task.
 * So the two things that arrive on other tasks (a list just fetched, a
 * removal heard on a bearer) are left here and taken in by the next request
 * that needs them: a removal matters when somebody is shown the room. */
#define PM_URL    "https://xprs.dev/moderators.json"
#define PM_TEXT   1024      /* the list as kept: npub,since,until;...  */
#define PM_FETCH  2048      /* a list is a handful of people            */
#define PM_HEARD  2
static portMUX_TYPE s_pm_mux = portMUX_INITIALIZER_UNLOCKED;
static char *s_pm_new;                       /* a list fetched, not taken in yet */
static char  s_pm_heard[PM_HEARD][XPRS_MAX_WIRE + 1];
static int   s_pm_nheard;
static volatile bool s_pm_fetching, s_pm_net;
static uint32_t s_pm_asked_s, s_pm_failed_s, s_pm_wait_s = 300;
static bool  s_pm_asked;

/* The value of "key" in the JSON object [obj, end), unquoted. */
static bool pm_field(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    char k[16];
    int kn = snprintf(k, sizeof k, "\"%s\"", key);
    out[0] = 0;
    for (const char *p = obj; p + kn < end; p++) {
        if (strncmp(p, k, (size_t)kn) != 0) continue;
        p += kn;
        while (p < end && (*p == ' ' || *p == ':')) p++;
        if (p >= end || *p != '"') return false;
        size_t o = 0;
        for (p++; p < end && *p != '"' && o < cap - 1; p++) out[o++] = *p;
        out[o] = 0;
        return true;
    }
    return false;
}

/* `2026-11-27` or a full XPRS timestamp, as epoch seconds; 0 for none. */
static uint32_t pm_day(const char *v)
{
    char t[24];
    size_t n = strlen(v);
    if (n == 10) snprintf(t, sizeof t, "%s_00:00:00", v);
    else if (n == 19) snprintf(t, sizeof t, "%s", v);
    else return 0;
    return xprsindex_ts_to_epoch(t, 19);
}

/* The published JSON as the line this station keeps. Returns how many
 * were listed, or -1 when [json] is not a list: a captive portal's page or
 * half a download must not leave the rooms with nobody, or with anybody. */
static int pm_compact(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"moderators\"");
    if (!p || !(p = strchr(p, '['))) return -1;
    const char *stop = strchr(p, ']');
    if (!stop) return -1;
    int n = 0;
    size_t o = 0;
    out[0] = 0;
    while (n < XWC_PMODS) {
        const char *obj = memchr(p, '{', (size_t)(stop - p));
        if (!obj) break;
        const char *end = memchr(obj, '}', (size_t)(stop - obj));
        if (!end) return -1;
        char npub[XWC_NPUB_LEN + 4], since[24], until[24], hrp[8];
        uint8_t key[40];
        size_t klen = sizeof key;
        p = end + 1;
        if (!pm_field(obj, end, "npub", npub, sizeof npub) || strlen(npub) >= XWC_NPUB_LEN) continue;
        if (bech32_decode(npub, hrp, key, &klen) != ESP_OK || klen != 32 || strcmp(hrp, "npub") != 0) continue;
        pm_field(obj, end, "since", since, sizeof since);
        pm_field(obj, end, "until", until, sizeof until);
        int w = snprintf(out + o, cap - o, "%s%s,%lu,%lu", n ? ";" : "", npub,
                         (unsigned long)pm_day(since), (unsigned long)pm_day(until));
        if (w <= 0 || (size_t)w >= cap - o) { out[o] = 0; break; }
        o += (size_t)w;
        n++;
    }
    return n;
}

/* Hand the kept line to the engine. */
static void pm_apply(const char *line)
{
    xwc_public_mods_clear();
    while (line && *line) {
        char npub[XWC_NPUB_LEN], hrp[8];
        uint8_t key[40];
        size_t klen = sizeof key, o = 0;
        unsigned long since = 0, until = 0;
        while (*line && *line != ',' && *line != ';' && o < sizeof npub - 1) npub[o++] = *line++;
        npub[o] = 0;
        if (*line == ',') sscanf(line, ",%lu,%lu", &since, &until);
        while (*line && *line != ';') line++;
        if (*line == ';') line++;
        if (bech32_decode(npub, hrp, key, &klen) == ESP_OK && klen == 32 && !strcmp(hrp, "npub"))
            xwc_public_mods_add(npub, key, (uint32_t)since, (uint32_t)until);
    }
}

void xapi_wc_public_poll(void)
{
    char *fresh = NULL;
    static char heard[PM_HEARD][XPRS_MAX_WIRE + 1];   /* this task only; not on its stack */
    int nh;
    portENTER_CRITICAL(&s_pm_mux);
    fresh = s_pm_new; s_pm_new = NULL;
    nh = s_pm_nheard; s_pm_nheard = 0;
    if (nh) memcpy(heard, s_pm_heard, sizeof heard);
    portEXIT_CRITICAL(&s_pm_mux);
    if (fresh) {
        kv_set("wc_pmods", fresh);
        pm_apply(fresh);
        ESP_LOGI(TAG, "public rooms: %d moderator(s) listed at xprs.dev", xwc_public_mods_count());
        free(fresh);
    }
    for (int i = 0; i < nh; i++)
        if (xwc_public_heard(heard[i], (int)strlen(heard[i])))
            ESP_LOGI(TAG, "public rooms: a listed moderator removed a message");
}

static bool pm_has(const char *w, int len, const char *word)
{
    int n = (int)strlen(word);
    for (int i = 0; i + n <= len; i++)
        if (w[i] == word[0] && memcmp(w + i, word, (size_t)n) == 0) return true;
    return false;
}

void xapi_wc_public_heard(const char *wire, int len)
{
    /* Every packet the station hears comes through here: one compare for
     * all but an act. The check itself waits for a visitor. */
    if (len < 12 || len > XPRS_MAX_WIRE || strncmp(wire, "t:moderate ", 11) != 0) return;
    if (pm_has(wire, len, " d:") || !pm_has(wire, len, " remove:message")) return;
    portENTER_CRITICAL(&s_pm_mux);
    if (s_pm_nheard >= PM_HEARD) {          /* the oldest gives way */
        memmove(s_pm_heard[0], s_pm_heard[1], sizeof s_pm_heard[0] * (PM_HEARD - 1));
        s_pm_nheard = PM_HEARD - 1;
    }
    memcpy(s_pm_heard[s_pm_nheard], wire, (size_t)len);
    s_pm_heard[s_pm_nheard++][len] = 0;
    portEXIT_CRITICAL(&s_pm_mux);
}

/* The site's certificate is checked against the roots its authority uses
 * (Let's Encrypt: ISRG Root X1 and X2), carried here because the boards are
 * built without the certificate bundle. Who moderates is not taken from a
 * server that did not prove it is xprs.dev; a board without the memory for
 * the handshake reads no list and honours nobody. */
static const char k_pm_roots[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n";

static void pm_fetch_task(void *arg)
{
    (void)arg;
    bool ok = false;
    int status = 0;     /* 0: no connection or no handshake */
    char *buf = malloc(PM_FETCH + 1);
    char *line = malloc(PM_TEXT);
    if (buf && line) {
        esp_http_client_config_t hc = {
            .url = PM_URL,
            .timeout_ms = 20000,
            .buffer_size = 1024,
            .cert_pem = k_pm_roots,
            .user_agent = "XPRS-ESP32/1.0",
        };
        esp_http_client_handle_t c = esp_http_client_init(&hc);
        if (c) {
            if (esp_http_client_open(c, 0) == ESP_OK) {
                esp_http_client_fetch_headers(c);
                int n = 0, got;
                while (n < PM_FETCH && (got = esp_http_client_read(c, buf + n, PM_FETCH - n)) > 0) n += got;
                buf[n > 0 ? n : 0] = 0;
                status = esp_http_client_get_status_code(c);
                if (status == 200 && n > 0 && n < PM_FETCH &&
                    pm_compact(buf, line, PM_TEXT) >= 0) {
                    char *old;
                    portENTER_CRITICAL(&s_pm_mux);
                    old = s_pm_new; s_pm_new = line;
                    portEXIT_CRITICAL(&s_pm_mux);
                    free(old);
                    line = NULL;
                    ok = true;
                }
                esp_http_client_close(c);
            }
            esp_http_client_cleanup(c);
        }
    }
    free(buf);
    free(line);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (ok) {
        s_pm_asked = true; s_pm_asked_s = up; s_pm_wait_s = 600;
    } else {
        /* Asked again later, and later still each time: a site that is down,
         * or a board that cannot make the handshake, is not asked at every
         * address its WiFi gets. */
        s_pm_failed_s = up ? up : 1;
        s_pm_wait_s = s_pm_wait_s * 2 > 12 * 3600 ? 12 * 3600 : s_pm_wait_s * 2;
        ESP_LOGW(TAG, "public rooms: the moderators' list was not read (status %d); the one kept stands", status);
    }
    s_pm_fetching = false;
    vTaskDelete(NULL);
}

void xapi_wc_net_up(void)
{
    /* An address before the engine started: asked when it starts. */
    s_pm_net = true;
    if (!s_buf || s_pm_fetching) return;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s_pm_asked && up - s_pm_asked_s < 12 * 3600) return;
    if (s_pm_failed_s && up - s_pm_failed_s < s_pm_wait_s) return;
    s_pm_fetching = true;
    if (xTaskCreate(pm_fetch_task, "pmods", 8192, NULL, 3, NULL) != pdPASS) s_pm_fetching = false;
}

int xapi_wc_gate(httpd_req_t *req, const char *wire, int len,
                 char id[7], char *why, size_t why_cap)
{
    xapi_wc_public_poll();
    return xwc_gate(wire, len, xapi_wc_local(req), id, why, why_cap);
}

void xapi_wc_start(httpd_handle_t h, const xprs_api_cfg_t *cfg)
{
    s_cfg = cfg;
    if (!s_buf) s_buf = malloc(WC_BUF);
    if (!s_buf) {
        ESP_LOGE(TAG, "no room for the web chat engine; not starting");
        return;
    }
    kv_load();
    xfer_start();
    /* The file store's directory, once, rather than on a visitor's request. */
    struct stat ds;
    if (stat(file_dir(), &ds) != 0) mkdir(file_dir(), 0775);
    static const xwc_cb_t cb = {
        .now = now_cb, .uptime = uptime_cb, .peer_key = peer_key_cb, .random_bytes = random_cb,
        .kv_get = kv_get, .kv_set = kv_set, .air = air_cb, .file = file_cb, .bind = bind_cb,
        .acts_path = "/idx/wcacts.txt", .locate = locate_cb,
    };
    const nostr_keys_t *k = nostr_keys_available() ? nostr_keys_get() : NULL;
    xwc_init(&cb, cfg->callsign, k ? k->private_key : NULL);
    int n = xwc_load();
    pm_apply(kv_get("wc_pmods"));
    static const httpd_uri_t uris[] = {
        { .uri = "/api/session", .method = HTTP_GET,  .handler = h_session_get },
        { .uri = "/api/session", .method = HTTP_POST, .handler = h_session_post },
        { .uri = "/api/admin",   .method = HTTP_GET,  .handler = h_admin_get },
        { .uri = "/api/admin",   .method = HTTP_POST, .handler = h_admin_post },
        { .uri = "/api/rooms",   .method = HTTP_GET,  .handler = h_rooms_get },
        { .uri = "/api/rooms",   .method = HTTP_POST, .handler = h_rooms_post },
        { .uri = "/api/xprs/file/*", .method = HTTP_GET,  .handler = h_file_get },
        { .uri = "/api/xprs/file/*", .method = HTTP_POST, .handler = h_file_post },
        { .uri = "/api/xprs/file/*", .method = HTTP_HEAD, .handler = h_file_get },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(h, &uris[i]);
    ESP_LOGI(TAG, "web chat engine up: %s, %d act(s) replayed, admin %s",
             cfg->callsign, n, xwc_admin_call() ? xwc_admin_call() : "none");
    if (s_pm_net) xapi_wc_net_up();
}
