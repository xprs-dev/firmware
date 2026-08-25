/**
 * @file xprs_api.c
 * @brief The station's HTTP API (spec/API-HTTP.md).
 */

#include "xprs_api.h"
#include "xapi_send.h"
#include "xprs_auth.h"
#include "xprs_ota.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "xprsindex.h"
#include "xprs.h"
#include <time.h>

static const char *TAG = "xprs_api";

static const xprs_api_cfg_t *s_cfg;
static httpd_handle_t s_httpd;

httpd_handle_t xprs_api_httpd(void) { return s_httpd; }

/* ---- small helpers ------------------------------------------------------ */

/* The one response buffer this server ever uses.
 *
 * Every handler here used to own a private static, or malloc per request:
 * eight buffers, 4,592 bytes of internal DRAM held whether or not anything
 * ever called the endpoint, on boards where the steady-state largest free
 * block is a couple of kilobytes. docs/esp32.md settled this once already
 * for the T-Dongle's own server ("One response buffer, claimed at boot and
 * shared") and this file did not follow it.
 *
 * Claimed before httpd_start, and if it cannot be had the server does not
 * start: a server that accepts a connection and then says nothing is the
 * hardest failure on this board to read from outside. esp_http_server runs
 * its handlers on ONE task, so one buffer is all there can ever be in use.
 */
#define API_BUF_SIZE 2048
static char *s_api_buf;

#define resp_json(req)              xapi_resp_json(req)
#define resp_error(req, st, why)    xapi_resp_error((req), (st), (why))
#define jesc(o, c, i, l)            xapi_jesc((o), (c), (i), (l))

static bool time_synced(void) { return time(NULL) > 1700000000; }

/* since/until: epoch seconds, or the packet's own YYYY-MM-DD_hh:mm:ss. */
static uint32_t parse_when(const char *v)
{
    if (!v || !v[0]) return 0;
    bool digits = true;
    for (const char *c = v; *c; c++)
        if (!isdigit((unsigned char)*c)) { digits = false; break; }
    if (digits) return (uint32_t)strtoul(v, NULL, 10);
    return xprsindex_ts_to_epoch(v, (int)strlen(v));
}

/* Base-callsign compare: "X1RD89-7" matches "X1RD89" (section 3.1). */
static bool call_matches(const char *want, const char *have)
{
    size_t n = 0;
    while (want[n] && want[n] != '-') n++;
    if (strncasecmp(want, have, n) != 0) return false;
    return have[n] == 0 || have[n] == '-';
}

/* ---- /api/status --------------------------------------------------------- */

static esp_err_t h_status(httpd_req_t *req)
{
    char *buf = s_api_buf;              /* shared; see API_BUF_SIZE */
    const size_t cap = API_BUF_SIZE;
    int n = snprintf(buf, cap,
        "{\"ok\":true,\"app\":\"%s\",\"board\":\"%s\",\"callsign\":\"%s\","
        "\"uptime_s\":%lu,\"heap_free\":%u,"
        "\"time\":{\"synced\":%s,\"epoch\":%lu,\"tz\":\"%s\"}",
        s_cfg->app, s_cfg->board, s_cfg->callsign,
        (unsigned long)(esp_timer_get_time() / 1000000),
        /* Internal: see the note in the diag handler. */
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        time_synced() ? "true" : "false",
        time_synced() ? (unsigned long)time(NULL) : 0,
        s_cfg->tz ? s_cfg->tz : "+00:00");
    if (s_cfg->status_json) {
        n += snprintf(buf + n, cap - n, ",");
        n += s_cfg->status_json(buf + n, cap - n);
    }
    if (s_cfg->index) {
        xprsidx_stats_t st;
        xprsindex_stats(s_cfg->index, &st);
        n += snprintf(buf + n, cap - n,
                      ",\"indexer\":{\"enabled\":true,\"count\":%lu,"
                      "\"epoch\":\"%c\"}",
                      (unsigned long)st.count, st.epoch);
    }
    n += snprintf(buf + n, cap - n, "}");
    resp_json(req);
    return httpd_resp_send(req, buf, n);
}

/* ---- /api/services ------------------------------------------------------- */

/* What the archiver is doing right now, for a laptop rather than a console. */
static esp_err_t h_peers(httpd_req_t *req)
{
    char *buf = s_api_buf;
    const size_t cap = API_BUF_SIZE;
    int n = snprintf(buf, cap, "{\"ok\":true");
    if (s_cfg->peers_json && n < (int)cap)
        n += s_cfg->peers_json(buf + n, cap - n);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "}");
    resp_json(req);
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_services(httpd_req_t *req)
{
    /* The board writes its two fragments straight into the answer where
     * they belong, the way status_json already does. Composing them in
     * separate slices of this same buffer and then printing one into the
     * other is what -Wrestrict objects to, and it needs the slices. */
    char *buf = s_api_buf;
    const size_t cap = API_BUF_SIZE;
    int n = snprintf(buf, cap, "{\"ok\":true,\"serve\":[");
    if (s_cfg->serve_json && n < (int)cap)
        n += s_cfg->serve_json(buf + n, cap - n);
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "],\"features\":{");
    if (s_cfg->features_json && n < (int)cap)
        n += s_cfg->features_json(buf + n, cap - n);
    if (n < (int)cap)
        n += snprintf(buf + n, cap - n,
            "},\"api\":[\"status\",\"services\",\"history\",\"mail\","
            "\"send\",\"log\"]}");
    resp_json(req);
    return httpd_resp_send(req, buf, n);
}

/* ---- /api/xprs/history and /api/xprs/mail -------------------------------- */

typedef struct {
    httpd_req_t *req;
    int emitted;
    int dir;              /* 0 any, 1 in, 2 out */
    const char *mail_for; /* non-NULL: only mail held for this callsign */
} hist_ctx_t;

static bool hist_emit(const xprsidx_rec_t *rec, void *arg)
{
    hist_ctx_t *c = (hist_ctx_t *)arg;
    bool own = rec->flags & XI_F_OUTGOING;
    if (c->dir == 1 && own) return true;
    if (c->dir == 2 && !own) return true;
    if (c->mail_for) {
        if (!(rec->flags & XI_F_MAIL)) return true;
        if (!call_matches(c->mail_for, rec->to)) return true;
    }
    const char *sig = !(rec->flags & XI_F_SIGNED)   ? "none"
                      : (rec->flags & XI_F_VERIFIED) ? "verified"
                                                      : "unverified";
    /* One record at a time out of the shared buffer: the escaped packet in
     * the head, the row built after it. */
    char *row = s_api_buf;
    const size_t ROW = API_BUF_SIZE;
    int n = snprintf(row, ROW,
        "%s{\"ts\":%lu,\"bearer\":\"%s\",\"rssi\":%d,\"from\":\"%s\","
        "\"to\":\"%s\",\"type\":\"%s\",\"sig\":\"%s\",\"own\":%s,"
        "\"wire\":\"",
        c->emitted ? "," : "", (unsigned long)rec->ts,
        xprsidx_bearer_name(rec->bearer), (int)rec->rssi,
        rec->from, rec->to, xprsidx_type_name(rec->type), sig,
        own ? "true" : "false");
    if (n < 0 || n >= (int)ROW) return true;
    n += jesc(row + n, ROW - (size_t)n, rec->wire, rec->len);
    if (n + 3 < (int)ROW) { row[n++] = '"'; row[n++] = '}'; row[n] = 0; }
    c->emitted++;
    return httpd_resp_send_chunk(c->req, row, n) == ESP_OK;
}

static esp_err_t history_common(httpd_req_t *req, bool mail_only)
{
    if (!s_cfg->index) return resp_error(req, "404 Not Found", "no index");

    char query[256] = {0}, p[48], call[48] = {0};
    hist_ctx_t ctx = { .req = req };
    xprsidx_query_t q = { .type = -1, .newest_first = true, .limit = 30,
                          .trusted = true };
    httpd_req_get_url_query_str(req, query, sizeof query);
    if (httpd_query_key_value(query, "since", p, sizeof p) == ESP_OK)
        q.since_ts = parse_when(p);
    if (httpd_query_key_value(query, "until", p, sizeof p) == ESP_OK)
        q.until_ts = parse_when(p);
    if (httpd_query_key_value(query, "only", p, sizeof p) == ESP_OK)
        q.type = xprsidx_type_code(p);
    if (httpd_query_key_value(query, "limit", p, sizeof p) == ESP_OK)
        q.limit = (uint32_t)strtoul(p, NULL, 10);
    if (q.limit == 0 || q.limit > 200) q.limit = 200;
    if (httpd_query_key_value(query, "call", p, sizeof p) == ESP_OK) {
        snprintf(call, sizeof call, "%s", p);
        q.from = call;
    }
    if (httpd_query_key_value(query, "dir", p, sizeof p) == ESP_OK)
        ctx.dir = p[0] == 'i' ? 1 : p[0] == 'o' ? 2 : 0;
    char mailcall[48] = {0};
    if (mail_only) {
        if (httpd_query_key_value(query, "call", mailcall,
                                  sizeof mailcall) != ESP_OK || !mailcall[0])
            return resp_error(req, "400 Bad Request", "mail needs call=");
        ctx.mail_for = mailcall;
        q.from = NULL;               /* call names the RECIPIENT here */
    }

    xprsidx_stats_t st;
    xprsindex_stats(s_cfg->index, &st);
    char head[128];
    int n = snprintf(head, sizeof head,
                     "{\"ok\":true,\"held\":%lu,\"time\":\"%s\",\"rows\":[",
                     (unsigned long)st.count,
                     time_synced() ? "synced" : "unsynced");
    resp_json(req);
    httpd_resp_send_chunk(req, head, n);

    /* Take the store for the read and hand it straight back -- the writer
     * keeps accepting into RAM meanwhile (docs/esp32.md). */
    xprsindex_pause_writes(s_cfg->index, true);
    xprsindex_query(s_cfg->index, &q, hist_emit, &ctx);
    xprsindex_pause_writes(s_cfg->index, false);

    char tail[48];
    n = snprintf(tail, sizeof tail, "],\"count\":%d}", ctx.emitted);
    httpd_resp_send_chunk(req, tail, n);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_history(httpd_req_t *req)
{
    return history_common(req, false);
}
static esp_err_t h_mail(httpd_req_t *req)
{
    return history_common(req, true);
}

/* ---- /api/xprs/send ------------------------------------------------------ */

/* The door lives in xapi_send.c so a board with its own server can take
 * just that object (see the note there). Here it borrows this server's
 * one buffer, like every other handler. */
static esp_err_t h_send(httpd_req_t *req)
{
    return xprs_api_send_handler(req, s_api_buf, API_BUF_SIZE,
                                 s_cfg->send_wire);
}

/* ---- /api/log ------------------------------------------------------------ */

/* Emit one log line as JSON. The stored line starts with the timestamp the
 * hook stamped: epoch digits, or +ms-since-boot. */
static bool log_emit(httpd_req_t *req, const char *line, int len, bool first)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len <= 0) return true;

    char tbuf[20] = "0";
    int i = 0;
    if (line[0] == '+' || isdigit((unsigned char)line[0])) {
        int o = 0;
        while (i < len && o < (int)sizeof tbuf - 1 && line[i] != ' ')
            tbuf[o++] = line[i++];
        tbuf[o] = 0;
        while (i < len && line[i] == ' ') i++;
    }
    /* The httpd task's stack is 6 KB here and 5 KB on the dongle, and
     * docs/esp32.md is explicit that a couple of 600-byte buffers on it
     * take the server down. These two live in the shared buffer's tail;
     * log_reversed owns the head while it walks. */
    char *row = s_api_buf + API_BUF_SIZE - 800;
    const size_t ROW = 800;
    int n = tbuf[0] == '+'
          ? snprintf(row, ROW, "%s{\"t\":\"%s\",\"m\":\"", first ? "" : ",", tbuf)
          : snprintf(row, ROW, "%s{\"t\":%s,\"m\":\"", first ? "" : ",", tbuf);
    if (n < 0 || n >= (int)ROW) return true;
    n += jesc(row + n, ROW - (size_t)n, line + i, len - i);
    if (n + 3 < (int)ROW) { row[n++] = '"'; row[n++] = '}'; row[n] = 0; }
    return httpd_resp_send_chunk(req, row, n) == ESP_OK;
}

/* Walk one file backwards in fixed blocks, newest line first. */
static int log_reversed(httpd_req_t *req, const char *path, int budget,
                        bool *first)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long pos = ftell(f);

    /* Head of the shared buffer: 512-byte block plus the carry, while
     * log_emit uses the tail. 1,248 bytes total, no stack, no malloc. */
    char *carry = s_api_buf;
    char *blk = s_api_buf + 200;
    const int CARRY = 200;
    int carry_n = 0, sent = 0;
    while (pos > 0 && budget > 0) {
        int n = pos > 512 ? 512 : (int)pos;
        pos -= n;
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(blk, 1, n, f) != (size_t)n) break;
        memcpy(blk + n, carry, carry_n);
        int total = n + carry_n;

        int end = total, i = total;
        while (i > 0 && budget > 0) {
            i--;
            if (blk[i] != '\n') continue;
            if (end > i + 1) {
                if (!log_emit(req, blk + i + 1, end - i - 1, *first)) budget = 0;
                else { *first = false; sent++; budget--; }
            }
            end = i;
        }
        if (pos == 0) {
            if (end > 0 && budget > 0 &&
                log_emit(req, blk, end, *first)) {
                *first = false; sent++; budget--;
            }
        } else {
            carry_n = end > CARRY ? CARRY : end;
            memcpy(carry, blk + end - carry_n, carry_n);
        }
    }
    fclose(f);
    return sent;
}

static esp_err_t h_log(httpd_req_t *req)
{
    char query[64] = {0}, p[16];
    int limit = 50;
    httpd_req_get_url_query_str(req, query, sizeof query);
    if (httpd_query_key_value(query, "limit", p, sizeof p) == ESP_OK)
        limit = atoi(p);
    if (limit <= 0 || limit > 500) limit = 500;

    resp_json(req);
    char head[80];
    int n = snprintf(head, sizeof head,
                     "{\"ok\":true,\"time\":\"%s\",\"lines\":[",
                     time_synced() ? "synced" : "unsynced");
    httpd_resp_send_chunk(req, head, n);
    bool first = true;
    int sent = log_reversed(req, s_cfg->log_cur, limit, &first);
    if (sent < limit)
        log_reversed(req, s_cfg->log_prev, limit - sent, &first);
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- start ---------------------------------------------------------------- */

/* Everything a person on a ladder would have gone up to read (25.8, and
 * docs/device.md): what is running, whether it is on probation, why it
 * last restarted, what the heap has been down to, and where an install
 * got to. Read-only, so no gate -- a station's health is not a secret. */
static esp_err_t h_diag(httpd_req_t *req)
{
    resp_json(req);
    /* The shared buffer, like every handler here. This was a malloc and a
     * free per request on the endpoint everything polls -- and then, worse,
     * a private static of its own, which is 1.1 KB of DRAM held for ever on
     * the board least able to spare it. */
    char *out = s_api_buf;
    const size_t cap = API_BUF_SIZE;
    esp_core_dump_summary_t cd_buf;     /* ~212 B, and only while asked */
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);

    esp_core_dump_summary_t *cd = &cd_buf;
    bool have_cd = esp_core_dump_get_summary(cd) == ESP_OK;

    int n = snprintf(out, cap,
        "{\"ok\":true,"
        /* Who and what, first: the push tooling reads these from every board
         * with an updater, and the dongle's diag already had them. */
        "\"callsign\":\"%s\",\"board\":\"%s\","
        "\"fw\":{\"version\":\"%s\",\"project\":\"%s\",\"idf\":\"%s\","
        "\"built\":\"%s %s\"},"
        "\"boot\":{\"reason\":%d,\"uptime_s\":%u},"
        "\"heap\":{\"free\":%u,\"largest\":%u,\"min_ever\":%u},"
        "\"part\":{\"running\":\"%s\",\"state\":%d,\"rollback\":%s},"
        "\"ota\":{\"busy\":%s,\"pct\":%d},"
        "\"psram\":%u",
        s_cfg->callsign ? s_cfg->callsign : "", s_cfg->board ? s_cfg->board : "",
        d ? d->version : "?", d ? d->project_name : "?",
        d ? d->idf_ver : "?", d ? d->date : "?", d ? d->time : "?",
        (int)esp_reset_reason(),
        (unsigned)(esp_timer_get_time() / 1000000ULL),
        /* INTERNAL, all three. They used to be free=total, largest=internal,
         * min_ever=total, which on a PSRAM board reads as 8.3 MB free with a
         * 17 KB largest block -- three numbers about two different memories
         * in one object. Internal is the one that decides whether a task
         * stack, a DMA buffer or a response buffer can be had; the PSRAM
         * total is reported separately below where it cannot be mistaken for
         * headroom. Same values as before on a board without PSRAM. */
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        run ? run->label : "?", (int)st,
        esp_ota_check_rollback_is_possible() ? "true" : "false",
        xota_busy() ? "true" : "false", xota_progress(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (have_cd && n > 0 && n < (int)cap)
        n += snprintf(out + n, cap - n,
                      ",\"crash\":{\"task\":\"%s\",\"pc\":\"0x%08lx\"}",
                      cd->exc_task, (unsigned long)cd->exc_pc);
    if (n > 0 && n < (int)cap)
        n += snprintf(out + n, cap - n, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, n);
}

/* /api/update lives in common/xprs_ota/xota_http.c -- one door for every
 * board, registered on this server in xprs_api_start(). */

/* The crash itself, not the one-line summary: whatever the panic wrote,
 * streamed out as the ELF espcoredump.py wants. Read in 1 KB pieces --
 * a 64 KB allocation on a board with this much free heap is how the
 * diagnosis tool becomes the second crash.
 *
 * Pair it with the .elf CI publishes beside the .bin, or the addresses
 * mean nothing:
 *   espcoredump.py info_corefile -t elf -c cd.elf xprs-<board>-<ver>.elf
 */
static esp_err_t h_coredump(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || !size)
        return resp_error(req, "404 Not Found", "no crash recorded");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) return resp_error(req, "404 Not Found", "no coredump partition");

    char disp[96];
    snprintf(disp, sizeof disp, "attachment; filename=\"coredump-%s-%s.elf\"",
             s_cfg->board ? s_cfg->board : "esp32", xota_version());
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char *buf = s_api_buf;              /* the shared buffer as the chunk */
    size_t off = 0;
    while (off < size) {
        size_t want = size - off > 1024 ? 1024 : size - off;
        if (esp_partition_read(part, off, buf, want) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, buf, want) != ESP_OK) return ESP_FAIL;
        off += want;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t xprs_api_start(const xprs_api_cfg_t *cfg)
{
    if (s_httpd) return ESP_OK;
    if (!cfg || !cfg->app || !cfg->callsign) return ESP_ERR_INVALID_ARG;
    s_cfg = cfg;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    hc.core_id = 1;          /* handlers read flash; keep off the radio core */
    hc.stack_size = 6144;
    /* A firmware push is well over a megabyte arriving while this same
     * task erases and writes flash, and an erase stops the cache for both
     * cores. The default five-second socket wait is sized for a JSON
     * request; on the T-Dongle two pushes died mid-transfer with recv=-3
     * before this was raised. Thirty seconds still reaps a dead client. */
    hc.recv_wait_timeout = 30;
    hc.send_wait_timeout = 30;
    hc.max_uri_handlers = 16;
    hc.lru_purge_enable = true;
    /* Claimed here, while the heap is still one large block, and never
     * released. A server that cannot answer is worse than one that never
     * started, so this failing is fatal to the server rather than to the
     * first request that needs it. */
    if (!s_api_buf) s_api_buf = malloc(API_BUF_SIZE);
    if (!s_api_buf) {
        ESP_LOGE(TAG, "no room for the %d-byte response buffer; not starting",
                 API_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = httpd_start(&s_httpd, &hc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "API failed to start: %s", esp_err_to_name(ret));
        return ret;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
        { .uri = "/api/services", .method = HTTP_GET, .handler = h_services },
        { .uri = "/api/xprs/history", .method = HTTP_GET, .handler = h_history },
        { .uri = "/api/xprs/peers", .method = HTTP_GET, .handler = h_peers },
        { .uri = "/api/xprs/mail", .method = HTTP_GET, .handler = h_mail },
        { .uri = "/api/xprs/send", .method = HTTP_POST, .handler = h_send },
        { .uri = "/api/xprs/send", .method = HTTP_GET, .handler = h_send },
        { .uri = "/api/log", .method = HTTP_GET, .handler = h_log },
        { .uri = "/api/diag", .method = HTTP_GET, .handler = h_diag },
        { .uri = "/api/coredump", .method = HTTP_GET, .handler = h_coredump },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(s_httpd, &uris[i]);
    ESP_LOGI(TAG, "HTTP API up on port 80 (spec/API-HTTP.md)");
    xota_http_register(s_httpd, s_cfg->callsign);
    return ESP_OK;
}
