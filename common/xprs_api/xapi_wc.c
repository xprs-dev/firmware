/* xapi_wc.c -- see the header. */
#include "xapi_wc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nostr_keys.h"
#include "xapi_send.h"
#include "xprsindex.h"
#include "xwc.h"

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

static bool file_cb(const char *wire, int len)
{
    if (!s_cfg || !s_cfg->index) return false;
    return xprsindex_add2(s_cfg->index, wire, len, 0, false, now_cb(), XI_B_LAN);
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
    } else if (sa.ss_family == AF_INET6) {
        const uint8_t *a = ((struct sockaddr_in6 *)&sa)->sin6_addr.s6_addr;
        static const uint8_t v4map[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
        if (memcmp(a, v4map, 12) == 0) {
            ip = ((uint32_t)a[12] << 24) | ((uint32_t)a[13] << 16) | ((uint32_t)a[14] << 8) | a[15];
        } else {
            if ((a[0] & 0xfe) == 0xfc) return true;          /* fc00::/7 */
            if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true; /* fe80::/10 */
            static const uint8_t lo[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
            return memcmp(a, lo, 16) == 0;
        }
    } else return false;
    uint8_t o0 = ip >> 24, o1 = (ip >> 16) & 255;
    return o0 == 127 || o0 == 10 || (o0 == 172 && o1 >= 16 && o1 < 32) ||
           (o0 == 192 && o1 == 168) || (o0 == 169 && o1 == 254) ||
           (o0 == 100 && o1 >= 64 && o1 < 128);
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
    char out[128];
    snprintf(out, sizeof out, "{\"ok\":true,\"session\":\"%s\",\"call\":\"%s\"}", token, call);
    return answer(req, 200, out);
}

/* ---- /api/admin ----------------------------------------------------- */
static esp_err_t h_admin_get(httpd_req_t *req)
{
    char token[XWC_TOKEN_LEN + 8];
    query_val(req, "session", token, sizeof token);
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
static esp_err_t h_rooms_get(httpd_req_t *req)
{
    char token[XWC_TOKEN_LEN + 8];
    query_val(req, "session", token, sizeof token);
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

/* ---- the hooks ------------------------------------------------------ */
int xapi_wc_services(char *buf, size_t cap)
{
    /* Every panel the board serves: the page hides what is not listed. */
    return xwc_services_json(buf, cap, "chat,traffic,devices,stats,station,settings,alerts");
}

int xapi_wc_gate(httpd_req_t *req, const char *wire, int len,
                 char id[7], char *why, size_t why_cap)
{
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
    static const xwc_cb_t cb = {
        .now = now_cb, .uptime = uptime_cb, .peer_key = peer_key_cb, .random_bytes = random_cb,
        .kv_get = kv_get, .kv_set = kv_set, .air = air_cb, .file = file_cb,
        .acts_path = "/idx/wcacts.txt",
    };
    const nostr_keys_t *k = nostr_keys_available() ? nostr_keys_get() : NULL;
    xwc_init(&cb, cfg->callsign, k ? k->private_key : NULL);
    int n = xwc_load();
    static const httpd_uri_t uris[] = {
        { .uri = "/api/session", .method = HTTP_GET,  .handler = h_session_get },
        { .uri = "/api/session", .method = HTTP_POST, .handler = h_session_post },
        { .uri = "/api/admin",   .method = HTTP_GET,  .handler = h_admin_get },
        { .uri = "/api/admin",   .method = HTTP_POST, .handler = h_admin_post },
        { .uri = "/api/rooms",   .method = HTTP_GET,  .handler = h_rooms_get },
        { .uri = "/api/rooms",   .method = HTTP_POST, .handler = h_rooms_post },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(h, &uris[i]);
    ESP_LOGI(TAG, "web chat engine up: %s, %d act(s) replayed, admin %s",
             cfg->callsign, n, xwc_admin_call() ? xwc_admin_call() : "none");
}
