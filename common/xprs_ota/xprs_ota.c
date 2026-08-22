/* xprs_ota.c -- see the header for the design and for what is deliberately
 * not defended. This file is the machinery. */
#include "xprs_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#if CONFIG_XPRS_OTA_PULL
#include "esp_http_client.h"
#include "esp_https_ota.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "xprs_config.h"
#include "xprssig.h"

static const char *TAG = "xota";

/* Where a request is parked while it waits for the worker, and where the
 * answer is parked while it waits for the NEXT BOOT to air it. */
#define XOTA_NVS "xota"

/* A transfer that has not finished in six minutes is not going to. Without
 * a wall-clock cap a server dribbling one byte every nine seconds feeds the
 * watchdog forever and the station never comes back. */
#define XOTA_MAX_SEC 360

static xota_cfg_t s_cfg;
static volatile bool s_pending;
static volatile bool s_busy;
static volatile int  s_pct = -1;

/* The pending request, filled by xota_request and read by the worker. */
static struct {
    char version[24];
    char url[160];
    char reply_to[16];
    char bearer[10];
    char cmd_id[8];
} s_req;

/* ── the signed line ─────────────────────────────────────────────────────
 * xprsfw1 <board> <version> <size> <sha256 hex>
 * Domain separation, and the binding of board+version+size to content. */
static void hexify(const uint8_t *in, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 15];
    }
    out[n * 2] = 0;
}

static bool unhex32(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* The pinned publisher key: config first, then the compiled-in default.
 * Both are re-writable with a cable, which is the point. */
static bool publisher_key(uint8_t out[32])
{
#ifdef XOTA_BENCH_PUBHEX
    /* A throwaway key for proving the door on a bench, compiled in and
     * never written to config -- so the real pinned key survives the test
     * and the next ordinary build is back on it. Announced at every use:
     * a shipped image with this flag would be a station anyone with the
     * bench key could reflash. */
    ESP_LOGE(TAG, "BENCH FIRMWARE KEY in use (XOTA_BENCH_PUBHEX) -- not for a roof");
    return unhex32(XOTA_BENCH_PUBHEX, out);
#endif
    const char *k = xcfg_get("fwkey", "");
    if (strlen(k) == 64 && unhex32(k, out)) return true;
    if (!k[0]) ESP_LOGW(TAG, "no firmware key pinned -- nothing can install");
    else       ESP_LOGW(TAG, "pinned firmware key is not 64 hex characters");
    return false;
}

/**
 * Is [sig85] a valid approval for these bytes, for THIS board and version?
 * Called before any flash work, so a forgery costs an erase of nothing.
 */
static bool approval_ok(const char *version, size_t size,
                        const uint8_t sha[32], const char *sig85)
{
    uint8_t pub[32];
    if (!publisher_key(pub)) return false;
    if (!sig85 || strlen(sig85) != XPRSSIG_B85_LEN) {
        ESP_LOGW(TAG, "approval is not %d base85 characters", XPRSSIG_B85_LEN);
        return false;
    }
    uint8_t sig[XPRSSIG_LEN];
    if (xprssig_b85_decode(sig85, XPRSSIG_B85_LEN, sig, sizeof sig)
        != XPRSSIG_LEN) {
        ESP_LOGW(TAG, "approval is not valid base85");
        return false;
    }

    char shahex[65];
    hexify(sha, 32, shahex);
    char line[160];
    int n = snprintf(line, sizeof line, "xprsfw1 %s %s %u %s",
                     s_cfg.board ? s_cfg.board : "?", version,
                     (unsigned)size, shahex);
    if (n <= 0 || n >= (int)sizeof line) return false;

    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)line, (size_t)n, digest, 0);
    bool ok = xprssig_verify(digest, sig, pub);
    if (!ok) ESP_LOGW(TAG, "approval does not verify for: %s", line);
    return ok;
}

/* ── parking an answer across a reboot ───────────────────────────────────
 * The 200 belongs to the firmware that proved itself, which is a different
 * boot from the one that accepted the command. So the command's id, who
 * asked, on which bearer, and what version was going in all go into NVS
 * before the reboot -- and whichever image comes up next reads them. */
static void park_result(const char *version)
{
    nvs_handle_t h;
    if (nvs_open(XOTA_NVS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "rid", s_req.cmd_id);
    nvs_set_str(h, "rto", s_req.reply_to);
    nvs_set_str(h, "rbear", s_req.bearer);
    nvs_set_str(h, "rver", version);
    nvs_commit(h);
    nvs_close(h);
}

static bool take_parked(char id[8], char to[16], char bearer[10], char ver[24])
{
    nvs_handle_t h;
    if (nvs_open(XOTA_NVS, NVS_READWRITE, &h) != ESP_OK) return false;
    size_t n;
    bool ok = true;
    n = 8;  if (nvs_get_str(h, "rid", id, &n) != ESP_OK) ok = false;
    n = 16; if (ok && nvs_get_str(h, "rto", to, &n) != ESP_OK) ok = false;
    n = 10; if (ok && nvs_get_str(h, "rbear", bearer, &n) != ESP_OK) ok = false;
    n = 24; if (ok && nvs_get_str(h, "rver", ver, &n) != ESP_OK) ok = false;
    if (ok) {
        nvs_erase_key(h, "rid");
        nvs_erase_key(h, "rto");
        nvs_erase_key(h, "rbear");
        nvs_erase_key(h, "rver");
        nvs_commit(h);
    }
    nvs_close(h);
    return ok;
}

static void air_result(const char *to, const char *bearer, const char *id,
                       int code, const char *fw, const char *msg)
{
    if (!s_cfg.air || !to || !to[0]) return;
    char ts[24] = "";
    time_t t = time(NULL);
    if (t > 1700000000) {
        struct tm tmv;
        gmtime_r(&t, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d_%H:%M:%S", &tmv);
    }
    char wire[251];
    int n = snprintf(wire, sizeof wire,
                     "t:result f:%s d:%s%s%s r:%s code:%d",
                     s_cfg.callsign ? s_cfg.callsign : "?", to,
                     ts[0] ? " ts:" : "", ts[0] ? ts : "", id, code);
    if (fw && fw[0] && n > 0 && n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " fw:%s", fw);
    if (msg && msg[0] && n > 0 && n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " m:%s", msg);
    if (n <= 0 || n >= (int)sizeof wire) return;
    s_cfg.air(bearer && bearer[0] ? bearer : "lan", wire, n);
}

const char *xota_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d && d->version[0] ? d->version : "0";
}

int  xota_progress(void) { return s_pct; }
bool xota_busy(void)     { return s_busy; }

/* Both doors hash the image as it streams past; only one of them exists
 * in any given build. */
static mbedtls_sha256_context s_sha;

#if CONFIG_XPRS_OTA_PULL
/* ── the stream tap ──────────────────────────────────────────────────────
 * esp_https_ota_finish() sets the boot partition unconditionally and gives
 * no hook in between, so the refusal has to happen earlier. The decrypt
 * callback runs BEFORE each esp_ota_write: hash there, and abort instead of
 * finishing when the hash disagrees. The buffer it hands back must be
 * heap-allocated -- IDF frees it after the write. */
static esp_err_t ota_tap(decrypt_cb_arg_t *a, void *ctx)
{
    (void)ctx;
    if (!a || !a->data_in || !a->data_in_len) return ESP_OK;
    mbedtls_sha256_update(&s_sha, (const unsigned char *)a->data_in,
                          a->data_in_len);
    void *copy = malloc(a->data_in_len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, a->data_in, a->data_in_len);
    a->data_out = copy;
    a->data_out_len = a->data_in_len;
    return ESP_OK;
}

/* ── the manifest ────────────────────────────────────────────────────────
 * Deliberately parsed with a scanner and a 2 KB buffer rather than a JSON
 * library: this runs on a board with about 13 KB of free heap. */
typedef struct {
    char     version[24];
    char     url[160];
    size_t   size;
    uint8_t  sha[32];
    char     sig[XPRSSIG_B85_LEN + 1];
} xota_manifest_t;

static bool json_field(const char *json, const char *key, char *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o < cap - 1) out[o++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && o < cap - 1) out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

static bool fetch_manifest(const char *base, const char *chan,
                           const char *want_version, xota_manifest_t *m)
{
    /* Two callers, two shapes. A channel means [base] is a feed root and
     * the manifest lives at the well-known place under it. No channel
     * means [base] IS the manifest, complete -- which is what an operator
     * naming a url: in cmd:update supplies. That second case used to fall
     * through the first and ask for
     * "http://host/thing.bin.json/firmware/m5stack-core/.json", so the
     * url: form of the command could never have worked. */
    char url[200];
    if (chan && chan[0])
        snprintf(url, sizeof url, "%s/firmware/%s/%s.json",
                 base, s_cfg.board ? s_cfg.board : "?", chan);
    else
        snprintf(url, sizeof url, "%s", base);
    ESP_LOGI(TAG, "manifest: %s", url);

    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return false;
    bool ok = false;
    /* Borrowed for the seconds this takes, not owned for the life of the
     * station: 2 KB of permanent .bss is a quarter of this board's free
     * heap, and the manifest is read once a month. */
    char *body = malloc(2048);
    if (!body) { esp_http_client_cleanup(c); return false; }
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int n = esp_http_client_read(c, body, 2047);
        if (n > 0) {
            body[n] = 0;
            char shahex[80] = "", sizestr[24] = "", rel[160] = "";
            ok = json_field(body, "version", m->version, sizeof m->version) &&
                 json_field(body, "sha256", shahex, sizeof shahex) &&
                 json_field(body, "size", sizestr, sizeof sizestr) &&
                 json_field(body, "sig", m->sig, sizeof m->sig) &&
                 json_field(body, "url", rel, sizeof rel);
            if (ok) ok = strlen(shahex) == 64 && unhex32(shahex, m->sha);
            if (ok) {
                m->size = (size_t)strtoul(sizestr, NULL, 10);
                if (strncmp(rel, "http", 4) == 0)
                    snprintf(m->url, sizeof m->url, "%s", rel);
                else
                    snprintf(m->url, sizeof m->url, "%.100s/%.55s", base, rel);
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    if (!ok) ESP_LOGW(TAG, "manifest unreadable or incomplete");
    else if (want_version && want_version[0] &&
             strcmp(want_version, m->version) != 0) {
        ESP_LOGW(TAG, "channel offers %s, %s was asked for",
                 m->version, want_version);
        ok = false;
    }
    return ok;
}

/* ── the install ─────────────────────────────────────────────────────── */
static xota_code_t do_install(const xota_manifest_t *m)
{
    /* Never chain: an image still on probation must prove itself before it
     * gets to choose the next one. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "this image is still on probation -- not installing");
        return XOTA_BUSY;
    }
    if (strcmp(m->version, xota_version()) == 0) return XOTA_UPTODATE;

    /* The refusal that costs nothing: verified before any flash work. */
    if (!approval_ok(m->version, m->size, m->sha, m->sig)) return XOTA_REFUSED;

    if (s_cfg.quiesce) s_cfg.quiesce(true);
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);

    esp_http_client_config_t hc = {
        .url = m->url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .buffer_size = 1024,
    };
    esp_https_ota_config_t oc = {
        .http_config = &hc,
        /* One bulk erase before the transfer instead of a sector erase every
         * few kilobytes throughout it: on the ESP32 an erase stalls the cache
         * for BOTH cores, and thousands of those spread over three minutes is
         * what takes the radios down. */
        .bulk_flash_erase = true,
        .decrypt_cb = ota_tap,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK || !h) {
        ESP_LOGE(TAG, "could not start: %s (free %u, largest %u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        mbedtls_sha256_free(&s_sha);
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return XOTA_FAILED;
    }

    /* The header says what this image thinks it is. A build for another
     * board would be caught by the signature anyway -- the board id is in
     * the signed line -- but failing here costs three fewer minutes. */
    esp_app_desc_t nd;
    if (esp_https_ota_get_img_desc(h, &nd) == ESP_OK) {
        const esp_app_desc_t *cur = esp_app_get_description();
        if (cur && strcmp(cur->project_name, nd.project_name) != 0) {
            ESP_LOGE(TAG, "image is for project %s, this is %s",
                     nd.project_name, cur->project_name);
            esp_https_ota_abort(h);
            mbedtls_sha256_free(&s_sha);
            if (s_cfg.quiesce) s_cfg.quiesce(false);
            return XOTA_REFUSED;
        }
    }

    esp_task_wdt_add(NULL);
    uint32_t t0 = (uint32_t)time(NULL);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        esp_task_wdt_reset();
        int got = esp_https_ota_get_image_len_read(h);
        s_pct = m->size ? (int)((int64_t)got * 100 / (int64_t)m->size) : 0;
        if ((uint32_t)time(NULL) - t0 > XOTA_MAX_SEC) {
            ESP_LOGE(TAG, "transfer gave up after %d s", XOTA_MAX_SEC);
            err = ESP_ERR_TIMEOUT;
            break;
        }
    }
    esp_task_wdt_delete(NULL);

    uint8_t got_sha[32];
    mbedtls_sha256_finish(&s_sha, got_sha);
    mbedtls_sha256_free(&s_sha);

    int len = esp_https_ota_get_image_len_read(h);
    bool complete = esp_https_ota_is_complete_data_received(h);
    if (err != ESP_OK || !complete || (size_t)len != m->size ||
        memcmp(got_sha, m->sha, 32) != 0) {
        ESP_LOGE(TAG, "refusing: %s (%d of %u bytes)",
                 err != ESP_OK ? esp_err_to_name(err)
                               : (complete ? "content did not match its digest"
                                           : "transfer incomplete"),
                 len, (unsigned)m->size);
        esp_https_ota_abort(h);      /* otadata untouched; the old image stays */
        s_pct = -1;
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return XOTA_FAILED;
    }

    /* Park the answer BEFORE the boot partition moves: after this point the
     * next thing that happens may be a reboot. */
    park_result(m->version);
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "finish failed: %s", esp_err_to_name(err));
        s_pct = -1;
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return XOTA_FAILED;
    }
    ESP_LOGW(TAG, "installed %s -- restarting", m->version);
    vTaskDelay(pdMS_TO_TICKS(400));   /* let the log line leave */
    esp_restart();
    return XOTA_ACCEPTED;             /* not reached */
}

void xota_poll(void)
{
    if (!s_pending) return;
    s_pending = false;
    {
        s_busy = true;
        s_pct = 0;

        /* Stand the station down BEFORE the manifest, not after.
         *
         * quiesce() used to be called inside do_install(), which is after
         * the manifest has already been fetched -- and the manifest fetch
         * is itself an HTTP client, a 2 KB buffer and a socket, on a board
         * that is standing down precisely because it has no room for them.
         * The M5Stack timed out at eight seconds with the server never
         * seeing the request. Everything from here to the end of the
         * attempt runs quiesced, and every exit below resumes. */
        if (s_cfg.quiesce) s_cfg.quiesce(true);

        xota_manifest_t m;
        memset(&m, 0, sizeof m);
        bool have = false;
        if (s_req.url[0]) {
            /* A named image still has to carry an approval, so the URL is
             * only ever a hint about where the bytes are. Its manifest is
             * the same file with .json appended. */
            char murl[200];
            snprintf(murl, sizeof murl, "%s.json", s_req.url);
            have = fetch_manifest(murl, NULL, s_req.version, &m);
            if (have) snprintf(m.url, sizeof m.url, "%s", s_req.url);
        } else {
            const char *base = xcfg_get("fwurl", "");
            const char *chan = xcfg_get("fwchan", "stable");
            if (base[0])
                have = fetch_manifest(base, chan,
                                      s_req.version[0] ? s_req.version : NULL,
                                      &m);
            else
                ESP_LOGW(TAG, "no update source configured");
        }

        xota_code_t code = have ? do_install(&m) : XOTA_FAILED;
        if (s_cfg.quiesce) s_cfg.quiesce(false);   /* idempotent */
        s_busy = false;
        s_pct = -1;
        /* Only a failure gets aired from here; a success has rebooted and
         * will answer from the other side of it. */
        if (code != XOTA_ACCEPTED && s_req.cmd_id[0]) {
            const char *why = code == XOTA_REFUSED  ? "not signed for this station"
                            : code == XOTA_UPTODATE ? "already running it"
                            : code == XOTA_BUSY     ? "still verifying the last one"
                                                    : "transfer failed";
            air_result(s_req.reply_to, s_req.bearer, s_req.cmd_id,
                       code == XOTA_UPTODATE ? 200 : (int)code,
                       xota_version(), why);
        }
        memset(&s_req, 0, sizeof s_req);
    }
}

#else  /* !CONFIG_XPRS_OTA_PULL -- a board that is given its image */

void xota_poll(void)
{
    if (!s_pending) return;
    s_pending = false;
    /* Nothing to fetch with. Say so rather than going quiet: an operator
     * who sent cmd:update to this station deserves to learn that this
     * board is push-only, not to watch a command evaporate. */
    ESP_LOGW(TAG, "asked to fetch an image; this build has no HTTP client");
    if (s_req.cmd_id[0])
        air_result(s_req.reply_to, s_req.bearer, s_req.cmd_id, 501,
                   xota_version(), "push the image to me");
    memset(&s_req, 0, sizeof s_req);
}

#endif /* CONFIG_XPRS_OTA_PULL */

esp_err_t xota_start(const xota_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    ESP_LOGI(TAG, "updater ready, running %s", xota_version());
    return ESP_OK;
}

xota_code_t xota_request(const char *version, const char *url,
                         const char *reply_to, const char *bearer,
                         const char *cmd_id)
{
    if (s_busy || s_pending) return XOTA_BUSY;
    memset(&s_req, 0, sizeof s_req);
    if (version)  snprintf(s_req.version,  sizeof s_req.version,  "%s", version);
    if (url)      snprintf(s_req.url,      sizeof s_req.url,      "%s", url);
    if (reply_to) snprintf(s_req.reply_to, sizeof s_req.reply_to, "%s", reply_to);
    if (bearer)   snprintf(s_req.bearer,   sizeof s_req.bearer,   "%s", bearer);
    if (cmd_id)   snprintf(s_req.cmd_id,   sizeof s_req.cmd_id,   "%s", cmd_id);
    s_pending = true;          /* the storage task picks it up */
    return XOTA_ACCEPTED;
}

/* ── the push door ───────────────────────────────────────────────────────
 * The same verification, minus the HTTP client: the caller already holds
 * the bytes (a phone on the station's own access point, or a laptop on the
 * bench). The approval is checked twice -- once on the announced digest
 * before the erase, once on what actually arrived. */
static struct {
    esp_ota_handle_t h;
    const esp_partition_t *part;
    char    version[24];
    char    sig[XPRSSIG_B85_LEN + 1];
    size_t  size, got;
    bool    open;
    bool verified;        /* verify() passed; commit() may restart */
} s_push;

esp_err_t xota_push_begin(const char *version, size_t size, const char *sig)
{
    if (s_busy || s_push.open) return ESP_ERR_INVALID_STATE;
    if (!version || !sig || !size) return ESP_ERR_INVALID_ARG;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;

    s_push.part = esp_ota_get_next_update_partition(NULL);
    if (!s_push.part) return ESP_ERR_NOT_FOUND;
    if (size > s_push.part->size) return ESP_ERR_INVALID_SIZE;

    snprintf(s_push.version, sizeof s_push.version, "%s", version);
    snprintf(s_push.sig, sizeof s_push.sig, "%s", sig);
    s_push.size = size;
    s_push.got = 0;

    if (s_cfg.quiesce) s_cfg.quiesce(true);
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    /* OTA_WITH_SEQUENTIAL_WRITES, not the announced size: with a size,
     * esp_ota_begin erases the whole 1.9 MB slot up front, and on this chip
     * an erase disables the cache for BOTH cores. The first push spent that
     * stall inside the HTTP worker and the UI task missed its watchdog. The
     * pull path can afford one long erase because it owns its task; a push
     * must keep answering a socket, so it erases as it writes. */
    esp_err_t err = esp_ota_begin(s_push.part, OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_push.h);
    if (err != ESP_OK) {
        mbedtls_sha256_free(&s_sha);
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return err;
    }
    s_push.open = true;
    s_busy = true;
    s_pct = 0;
    ESP_LOGI(TAG, "push of %s (%u bytes) into %s",
             version, (unsigned)size, s_push.part->label);
    return ESP_OK;
}

esp_err_t xota_push_write(const void *data, size_t len)
{
    if (!s_push.open) return ESP_ERR_INVALID_STATE;
    if (!len) return ESP_OK;
    if (s_push.got + len > s_push.size) return ESP_ERR_INVALID_SIZE;
    mbedtls_sha256_update(&s_sha, (const unsigned char *)data, len);
    esp_err_t err = esp_ota_write(s_push.h, data, len);
    if (err != ESP_OK) return err;
    s_push.got += len;
    /* Air for everybody else. Sixteen kilobytes of flash writing between
     * breaths keeps the screen, the bearers and the watchdog alive through
     * the three minutes this takes. */
    if ((s_push.got & 0x3FFF) < len) vTaskDelay(pdMS_TO_TICKS(2));
    s_pct = (int)((int64_t)s_push.got * 100 / (int64_t)s_push.size);
    return ESP_OK;
}

void xota_push_abort(void)
{
    if (!s_push.open) return;
    esp_ota_abort(s_push.h);
    mbedtls_sha256_free(&s_sha);
    memset(&s_push, 0, sizeof s_push);
    s_busy = false;
    s_pct = -1;
    if (s_cfg.quiesce) s_cfg.quiesce(false);
}

/* The end of a push is two steps on purpose. verify() answers "is this
 * image genuine and complete" and leaves the boot partition alone, so the
 * door can tell the pusher the truth in its HTTP reply; commit() then
 * switches the boot partition and restarts. finish() is both, for a caller
 * with nobody to answer. */
esp_err_t xota_push_verify(void)
{
    if (!s_push.open || s_push.verified) return ESP_ERR_INVALID_STATE;
    uint8_t sha[32];
    mbedtls_sha256_finish(&s_sha, sha);
    mbedtls_sha256_free(&s_sha);

    if (s_push.got != s_push.size ||
        !approval_ok(s_push.version, s_push.size, sha, s_push.sig)) {
        ESP_LOGE(TAG, "push refused: %s",
                 s_push.got != s_push.size ? "short" : "no valid approval");
        esp_ota_abort(s_push.h);
        memset(&s_push, 0, sizeof s_push);
        s_busy = false;
        s_pct = -1;
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = esp_ota_end(s_push.h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "push refused: image rejected by esp_ota_end (%s)",
                 esp_err_to_name(err));
        memset(&s_push, 0, sizeof s_push);
        s_busy = false;
        s_pct = -1;
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return err;
    }
    s_push.verified = true;
    return ESP_OK;
}

esp_err_t xota_push_commit(void)
{
    if (!s_push.open || !s_push.verified) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_set_boot_partition(s_push.part);
    if (err != ESP_OK) {
        memset(&s_push, 0, sizeof s_push);
        s_busy = false;
        s_pct = -1;
        if (s_cfg.quiesce) s_cfg.quiesce(false);
        return err;
    }
    park_result(s_push.version);
    ESP_LOGW(TAG, "pushed %s -- restarting", s_push.version);
    memset(&s_push, 0, sizeof s_push);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

esp_err_t xota_push_finish(void)
{
    esp_err_t err = xota_push_verify();
    return err == ESP_OK ? xota_push_commit() : err;
}

/* ── the two sides of a reboot ───────────────────────────────────────── */
void xota_mark_healthy(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;

    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "this image proved itself -- rollback cancelled");

    char id[8], to[16], bearer[10], ver[24];
    if (take_parked(id, to, bearer, ver) && strcmp(ver, xota_version()) == 0)
        air_result(to, bearer, id, 200, xota_version(), NULL);
}

void xota_mark_unhealthy(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGE(TAG, "this image did not come up -- going back to the one that did");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

bool xota_report_rollback(void)
{
    char id[8], to[16], bearer[10], ver[24];
    if (!take_parked(id, to, bearer, ver)) return false;
    if (strcmp(ver, xota_version()) == 0) {
        /* We ARE the version that was going in: this boot is the new image
         * on probation, and its answer belongs to xota_mark_healthy. Put it
         * back and say nothing yet. */
        snprintf(s_req.cmd_id, sizeof s_req.cmd_id, "%s", id);
        snprintf(s_req.reply_to, sizeof s_req.reply_to, "%s", to);
        snprintf(s_req.bearer, sizeof s_req.bearer, "%s", bearer);
        park_result(ver);
        memset(&s_req, 0, sizeof s_req);
        return false;
    }
    /* We are NOT that version, so the bootloader put us back. Say so --
     * this is the packet the whole feature exists for. */
    ESP_LOGW(TAG, "rolled back from %s: still running %s", ver, xota_version());
    air_result(to, bearer, id, 500, xota_version(), "rolled back");
    return true;
}
