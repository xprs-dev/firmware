/**
 * @file xprs_config.c
 * @brief Station configuration store + config.ini + browser editor.
 */

#include "xprs_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"

static const char *TAG = "xprs_config";

#define NVS_NS "xprscfg"

/* The known keys, cached in RAM. Values are small; the nsec is the largest
 * at 63 characters. */
typedef struct { const char *key; char val[80]; bool loaded; } cfg_entry_t;

static cfg_entry_t s_cfg[] = {
    { "name",      {0}, false },
    { "nsec",      {0}, false },
    { "wifi_on",   {0}, false },
    { "ssid",      {0}, false },
    { "pass",      {0}, false },
    { "espnow_on", {0}, false },
    { "share_on",  {0}, false },
    { "digi_on",   {0}, false },
    { "bridge_on", {0}, false },
    { "igate_on",  {0}, false },
    { "index_on",  {0}, false },
    /* A super-archiver (XPRS.md 36.9.4): keeps everything it is handed, is
     * asked by many stations, and sizes its spool to the volume rather than
     * to a pocket device's budget. Off by default -- claiming the word
     * commits a station to serving it. */
    { "index_super", {0}, false },
    /* Super-archivers this station pulls from by name, comma separated.
     * Discovery from a `serve:archive,super` beacon covers whoever is in
     * earshot; this covers the one on the far side of a hub, which a board
     * will never hear a beacon from (36.12.2). */
    { "supers",    {0}, false },
    { "ntp",       {0}, false },
    { "ap_on",     {0}, false },
    { "ap_ssid",   {0}, false },
    { "tz",        {0}, false },
    /* Over-the-air updates (XPRS.md 25.8) and who may ask for one (25.4).
     * `fwkey` is the x-only key whose signature makes an image installable
     * here; `own1..own4` are the callsign-deriving npubs allowed to command
     * this station. Both are re-writable with a cable on purpose: a lost
     * key must be a ladder, never a brick. */
    { "fwkey",     {0}, false },
    { "fwurl",     {0}, false },
    { "fwchan",    {0}, false },
    { "fwauto",    {0}, false },
    /* Scripts (common/xprs_script). A DIFFERENT publisher key from `fwkey`,
     * so "may publish panels for this station" can be delegated without also
     * delegating "may reflash it". Unset falls back to fwkey; with neither
     * set nothing verifies and no script runs. */
    { "scripts_on", {0}, false },
    { "scriptkey",  {0}, false },
    /* The Reticulum uplink (xprs_bearer_rns): where this archiver dials to
     * be reachable over RNS, and how hard it may lean on that link. */
    { "rns_hub",     {0}, false },
    { "ble_on",      {0}, false },
    { "rns_pace_ms", {0}, false },
    { "own1",      {0}, false },
    { "own2",      {0}, false },
    { "own3",      {0}, false },
    { "own4",      {0}, false },
};
#define CFG_N (sizeof(s_cfg) / sizeof(s_cfg[0]))

static cfg_entry_t *find(const char *key)
{
    for (size_t i = 0; i < CFG_N; i++)
        if (strcmp(s_cfg[i].key, key) == 0) return &s_cfg[i];
    return NULL;
}

esp_err_t xcfg_init(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   /* nothing saved yet */
    if (ret != ESP_OK) return ret;
    for (size_t i = 0; i < CFG_N; i++) {
        size_t len = sizeof(s_cfg[i].val);
        if (nvs_get_str(nvs, s_cfg[i].key, s_cfg[i].val, &len) == ESP_OK)
            s_cfg[i].loaded = true;
    }
    nvs_close(nvs);
    return ESP_OK;
}

const char *xcfg_get(const char *key, const char *def)
{
    cfg_entry_t *e = find(key);
    if (e && e->loaded && e->val[0]) return e->val;
    return def;
}

bool xcfg_get_bool(const char *key, bool def)
{
    const char *v = xcfg_get(key, NULL);
    if (!v) return def;
    return v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' ||
           v[0] == '1';
}

esp_err_t xcfg_set(const char *key, const char *value)
{
    cfg_entry_t *e = find(key);
    if (!e) return ESP_ERR_INVALID_ARG;
    if (!value) value = "";
    snprintf(e->val, sizeof e->val, "%s", value);
    e->loaded = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;
    if (value[0])
        ret = nvs_set_str(nvs, key, value);
    else
        ret = nvs_erase_key(nvs, key);   /* empty = gone */
    if (ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

esp_err_t xcfg_set_bool(const char *key, bool value)
{
    return xcfg_set(key, value ? "yes" : "no");
}

/* ---- config.ini --------------------------------------------------------- */

int xcfg_ini_render(char *buf, size_t cap)
{
    return snprintf(buf, cap,
        "; XPRS station configuration\n"
        "; Save this file and the station restarts with the new settings.\n"
        "\n"
        "[station]\n"
        "; A friendly name for this device.\n"
        "name = %s\n"
        "; Paste an nsec here to give the station that identity. The value\n"
        "; is consumed on restart and never shown back in this file.\n"
        "nsec = \n"
        "\n"
        "[wifi]\n"
        "enabled = %s\n"
        "ssid = %s\n"
        "password = %s\n"
        "\n"
        "[espnow]\n"
        "enabled = %s\n"
        "\n"
        "[digipeater]\n"
        "; Re-air packets heard on ESP-NOW back onto ESP-NOW.\n"
        "enabled = %s\n"
        "\n"
        "[bridge]\n"
        "; Carry LAN traffic onto the ESP-NOW radio.\n"
        "enabled = %s\n"
        "\n"
        "[igate]\n"
        "; Carry ESP-NOW traffic onto the LAN (toward the internet side).\n"
        "enabled = %s\n"
        "\n"
        "[indexer]\n"
        "; Keep every packet heard, answer cmd:history, hold mail.\n"
        "enabled = %s\n"
        "\n"
        "[share]\n"
        "; This browser editor itself.\n"
        "enabled = %s\n"
        "\n"
        "[hotspot]\n"
        "; The walk-up WiFi: an open network whose sign-in page is the chat.\n"
        "enabled = %s\n"
        "ssid = %s\n"
        "\n"
        "[time]\n"
        "; NTP server (community pool by default) and the timezone as an\n"
        "; offset from UTC, e.g. +01:00 or -05:30. The offset places the\n"
        "; day boundary for the daily statistics.\n"
        "server = %s\n"
        "tz = %s\n"
        "\n"
        "[update]\n"
        "; Firmware updates over the air (XPRS.md 25.8). `key` is the public\n"
        "; half whose signature makes an image installable on this station;\n"
        "; an image signed by anybody else is refused before a byte is\n"
        "; written. `source` is where to look, `channel` which line to\n"
        "; follow, and `auto` whether to check on a timer or only when asked.\n"
        "key = %s\n"
        "source = %s\n"
        "channel = %s\n"
        "auto = %s\n"
        "\n"
        "[owners]\n"
        "; npubs allowed to command this station: update, reboot, and any\n"
        "; other actuation. A command from anybody else is discarded, and one\n"
        "; from a station we can identify but do not permit gets a refusal.\n"
        "; Empty means nobody may -- the station still answers questions.\n"
        "one = %s\n"
        "two = %s\n"
        "three = %s\n"
        "four = %s\n",
        xcfg_get("name", ""),
        xcfg_get_bool("wifi_on", true) ? "yes" : "no",
        xcfg_get("ssid", ""),
        xcfg_get("pass", ""),
        xcfg_get_bool("espnow_on", true) ? "yes" : "no",
        xcfg_get_bool("digi_on", false) ? "yes" : "no",
        xcfg_get_bool("bridge_on", true) ? "yes" : "no",
        xcfg_get_bool("igate_on", true) ? "yes" : "no",
        xcfg_get_bool("index_on", true) ? "yes" : "no",
        xcfg_get_bool("share_on", false) ? "yes" : "no",
        xcfg_get_bool("ap_on", true) ? "yes" : "no",
        xcfg_get("ap_ssid", ""),
        xcfg_get("ntp", "pool.ntp.org"),
        xcfg_get("tz", "+00:00"),
        xcfg_get("fwkey", ""),
        xcfg_get("fwurl", ""),
        xcfg_get("fwchan", "stable"),
        xcfg_get_bool("fwauto", false) ? "yes" : "no",
        xcfg_get("own1", ""),
        xcfg_get("own2", ""),
        xcfg_get("own3", ""),
        xcfg_get("own4", ""));
}

/* section + key -> NVS key. */
static const struct { const char *sec, *ini, *key; } s_ini_map[] = {
    { "station", "name",     "name" },
    { "station", "nsec",     "nsec" },
    { "wifi",    "enabled",  "wifi_on" },
    { "wifi",    "ssid",     "ssid" },
    { "wifi",    "password", "pass" },
    { "espnow",  "enabled",  "espnow_on" },
    { "share",   "enabled",  "share_on" },
    { "digipeater", "enabled", "digi_on" },
    { "bridge",  "enabled",  "bridge_on" },
    { "igate",   "enabled",  "igate_on" },
    { "indexer", "enabled",  "index_on" },
    { "indexer", "super",    "index_super" },
    { "indexer", "supers",   "supers" },
    /* The hub this station DIALS OUT to. It was in the cache and nowhere in
     * this map, so it could only be set from a serial console -- which a box
     * beside a router does not have a cable to. A super-archiver is never
     * port-forwarded; it dials out, and this is the line that says where. */
    { "rns",     "hub",      "rns_hub" },
    /* Bluetooth. On by default, and the one thing a router-side station
     * turns off: the controller is what pays for the hub socket. */
    { "ble",     "enabled",  "ble_on" },
    { "hotspot", "enabled",  "ap_on" },
    { "hotspot", "ssid",     "ap_ssid" },
    { "time",    "server",   "ntp" },
    { "time",    "tz",       "tz" },
    { "update",  "key",      "fwkey" },
    { "update",  "source",   "fwurl" },
    { "update",  "channel",  "fwchan" },
    { "update",  "auto",     "fwauto" },
    /* Scripts are signed by a DIFFERENT key from firmware, on purpose. It
     * lets an operator delegate "may publish panels for this station" without
     * also delegating "may reflash the roof". Unset falls back to fwkey. */
    { "scripts", "enabled",  "scripts_on" },
    { "scripts", "key",      "scriptkey" },
    { "owners",  "one",      "own1" },
    { "owners",  "two",      "own2" },
    { "owners",  "three",    "own3" },
    { "owners",  "four",     "own4" },
};

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

esp_err_t xcfg_ini_apply(const char *text, size_t len)
{
    char sec[16] = "";
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[160];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        p = nl ? nl + 1 : end;

        /* Comments die, whole-line or trailing. */
        char *cm = line;
        while ((cm = strpbrk(cm, ";#")) != NULL) { *cm = 0; break; }
        char *ln = trim(line);
        if (!ln[0]) continue;

        if (ln[0] == '[') {
            char *cl = strchr(ln, ']');
            if (cl) {
                *cl = 0;
                snprintf(sec, sizeof sec, "%s", trim(ln + 1));
            }
            continue;
        }
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(ln), *v = trim(eq + 1);
        for (size_t i = 0; i < sizeof s_ini_map / sizeof s_ini_map[0]; i++) {
            if (strcasecmp(sec, s_ini_map[i].sec) == 0 &&
                strcasecmp(k, s_ini_map[i].ini) == 0) {
                /* A blank nsec line is the file's normal state, not an
                 * instruction to erase the stored identity. */
                if (strcmp(s_ini_map[i].key, "nsec") == 0 && !v[0]) break;
                xcfg_set(s_ini_map[i].key, v);
                break;
            }
        }
    }
    ESP_LOGI(TAG, "config.ini applied");
    return ESP_OK;
}

/* ---- the browser editor ------------------------------------------------- */

static httpd_handle_t s_httpd;
static bool s_own_server;     /* we started it, we stop it */
static bool s_registered;     /* the share's URIs are on the server */
static const char *s_log_cur, *s_log_prev;

void xcfg_share_attach(void *httpd_handle)
{
    if (s_registered) return;             /* attach before start */
    s_httpd = (httpd_handle_t)httpd_handle;
    s_own_server = false;
}

void xcfg_share_set_log(const char *current, const char *previous)
{
    s_log_cur = current;
    s_log_prev = previous;
}

/* Stream one file LINE-REVERSED: blocks read back to front, lines inside
 * each combined buffer emitted last-first, the partial line at a block's
 * start carried into the next round. Small fixed buffers -- the httpd
 * task's stack is no place for a file, and neither is the heap. */
static void send_file_reversed(httpd_req_t *req, const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long pos = ftell(f);

    char carry[200];
    int carry_n = 0;
    char blk[512 + sizeof carry];
    while (pos > 0) {
        int n = pos > 512 ? 512 : (int)pos;
        pos -= n;
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(blk, 1, n, f) != (size_t)n) break;
        memcpy(blk + n, carry, carry_n);
        int total = n + carry_n;

        /* Emit whole lines from the back; what precedes the first newline
         * belongs to a line that starts in an earlier block. */
        int end = total;
        int i = total;
        while (i > 0) {
            i--;
            if (blk[i] != '\n') continue;
            if (end > i + 1)
                httpd_resp_send_chunk(req, blk + i + 1, end - i - 1);
            httpd_resp_send_chunk(req, "\n", 1);
            end = i;
        }
        if (pos == 0) {
            if (end > 0) {
                httpd_resp_send_chunk(req, blk, end);
                httpd_resp_send_chunk(req, "\n", 1);
            }
        } else {
            carry_n = end > (int)sizeof carry ? (int)sizeof carry : end;
            /* Keep the TAIL of the partial line if it overflows the carry. */
            memcpy(carry, blk + end - carry_n, carry_n);
        }
    }
    fclose(f);
}

static esp_err_t h_log(httpd_req_t *req)
{
    /* Newest first: the current file backwards, then the previous one. */
    httpd_resp_set_type(req, "text/plain");
    send_file_reversed(req, s_log_cur);
    send_file_reversed(req, s_log_prev);
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_timer_handle_t s_restart_timer;

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>XPRS station</title>"
    "<style>body{font-family:monospace;background:#111;color:#ddd;"
    "max-width:720px;margin:2em auto;padding:0 1em}"
    "textarea{width:100%;height:24em;background:#000;color:#7f7;"
    "border:1px solid #444;padding:.6em;font:inherit}"
    "button{font:inherit;padding:.4em 1.4em;margin-top:.6em}</style>"
    "<h2>XPRS station config</h2>"
    "<p>Edit config.ini and press save. The station restarts with the new "
    "settings.</p>"
    "<textarea id=t spellcheck=false></textarea><br>"
    "<button onclick=save()>Save + restart</button> <span id=m></span>"
    "<script>"
    "fetch('/config.ini').then(r=>r.text()).then(t=>{document.getElementById('t').value=t});"
    "function save(){fetch('/config.ini',{method:'POST',"
    "body:document.getElementById('t').value}).then(r=>r.text())"
    ".then(t=>{document.getElementById('m').textContent=t})}"
    "</script>";

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_get_ini(httpd_req_t *req)
{
    /* Measured, then sized. This used to render into 1,024 bytes on the
     * httpd task's stack and send snprintf's RETURN value -- the length the
     * file would have had -- so once the template outgrew the buffer (a
     * 64-character fwkey and four owner lines did it) every download was
     * truncated at [time] and followed by seven hundred bytes of whatever
     * lay above that buffer on the stack. Over HTTP, to anyone on the LAN
     * while the share was on. */
    int need = xcfg_ini_render(NULL, 0);
    if (need < 0) return httpd_resp_send_500(req);
    char *buf = malloc((size_t)need + 1);
    if (!buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "no memory for config.ini right now\n");
    }
    int n = xcfg_ini_render(buf, (size_t)need + 1);
    if (n > need) n = need;
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t h_post_ini(httpd_req_t *req)
{
    /* On the heap: the httpd task's stack is no place for a file. */
    char *body = malloc(2048);
    if (!body) return httpd_resp_send_500(req);
    int total = 0;
    while (total < req->content_len && total < 2047) {
        int r = httpd_req_recv(req, body + total, 2047 - total);
        if (r <= 0) { free(body); return httpd_resp_send_500(req); }
        total += r;
    }
    body[total] = 0;
    xcfg_ini_apply(body, total);
    free(body);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Saved. Restarting...", HTTPD_RESP_USE_STRLEN);
    /* Let the reply reach the browser, then come back up with the new
     * settings applied. */
    if (!s_restart_timer) {
        const esp_timer_create_args_t args = {
            .callback = restart_cb, .name = "cfg_restart",
        };
        esp_timer_create(&args, &s_restart_timer);
    }
    esp_timer_start_once(s_restart_timer, 1500000);
    return ESP_OK;
}

esp_err_t xcfg_share_start(void)
{
    if (s_registered) return ESP_OK;
    if (!s_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.core_id = 1;      /* handlers read flash (docs/esp32.md) */
        cfg.stack_size = 6144;
        cfg.lru_purge_enable = true;
        esp_err_t ret = httpd_start(&s_httpd, &cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "editor failed to start: %s", esp_err_to_name(ret));
            return ret;
        }
        s_own_server = true;
    }
    static const httpd_uri_t u_page = { .uri = "/config", .method = HTTP_GET,
                                        .handler = h_page };
    static const httpd_uri_t u_get  = { .uri = "/config.ini",
                                        .method = HTTP_GET,
                                        .handler = h_get_ini };
    static const httpd_uri_t u_post = { .uri = "/config.ini",
                                        .method = HTTP_POST,
                                        .handler = h_post_ini };
    static const httpd_uri_t u_log = { .uri = "/log.txt", .method = HTTP_GET,
                                       .handler = h_log };
    httpd_register_uri_handler(s_httpd, &u_page);
    httpd_register_uri_handler(s_httpd, &u_get);
    httpd_register_uri_handler(s_httpd, &u_post);
    httpd_register_uri_handler(s_httpd, &u_log);
    s_registered = true;
    ESP_LOGI(TAG, "config editor serving on port 80");
    return ESP_OK;
}

void xcfg_share_stop(void)
{
    if (!s_registered) return;
    if (s_own_server) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        s_own_server = false;
    } else {
        /* A shared server keeps running; only the share's doors close. */
        httpd_unregister_uri_handler(s_httpd, "/config", HTTP_GET);
        httpd_unregister_uri_handler(s_httpd, "/config.ini", HTTP_GET);
        httpd_unregister_uri_handler(s_httpd, "/config.ini", HTTP_POST);
        httpd_unregister_uri_handler(s_httpd, "/log.txt", HTTP_GET);
    }
    s_registered = false;
    ESP_LOGI(TAG, "config editor stopped");
}

bool xcfg_share_running(void)
{
    return s_registered;
}


/* ---- the cable: cfg get/set/del on a serial console ---------------------- */

/* Shape rules for values that are keys. own1..own4 hold an npub (63 chars,
 * bech32 charset); every *key (fwkey, the script publisher) holds 64 hex. */
static const char *key_shape_error(const char *key, const char *val)
{
    size_t n = strlen(val);
    if (strncmp(key, "own", 3) == 0) {
        if (n != 63 || strncmp(val, "npub1", 5) != 0) return "an npub is 63 chars starting npub1";
        for (const char *c = val + 5; *c; c++)
            if (!strchr("qpzry9x8gf2tvdw0s3jn54khce6mua7l", *c)) return "not a bech32 npub";
    } else if (n > 3 && strcmp(key + strlen(key) - 3, "key") == 0) {
        if (n != 64) return "a key is 64 hex chars";
        for (const char *c = val; *c; c++)
            if (!isxdigit((unsigned char)*c)) return "a key is 64 hex chars";
    }
    return NULL;
}

bool xcfg_console(const char *line)
{
    if (!line || strncmp(line, "cfg", 3) != 0) return false;
    if (line[3] != ' ' && line[3] != 0) return false;
    const char *p = line + 3;
    while (*p == ' ') p++;
    if (!*p) {
        printf("cfg get <key> | cfg set <key> <value> | cfg del <key>\n");
        return true;
    }
    char verb[8] = "", key[32] = "";
    int n = 0;
    if (sscanf(p, "%7s %31s %n", verb, key, &n) < 2) {
        printf("cfg: need a verb and a key\n");
        return true;
    }
    const char *val = p + n;
    if (strcmp(verb, "get") == 0) {
        const char *v = xcfg_get(key, "");
        printf("%s=%s\n", key, v);
    } else if (strcmp(verb, "set") == 0) {
        if (!*val) { printf("cfg set %s: no value\n", key); return true; }
        /* A key typed over a serial console can arrive with bytes missing
         * (USB-JTAG drops them on a long line), and a mangled owner or
         * publisher key locks the board to nobody. Check the shape first. */
        const char *bad = key_shape_error(key, val);
        if (bad) { printf("cfg set %s: refused, %s\n", key, bad); return true; }
        esp_err_t e = xcfg_set(key, val);
        printf("%s=%s (%s)\n", key, val, e == ESP_OK ? "saved" : esp_err_to_name(e));
    } else if (strcmp(verb, "del") == 0) {
        esp_err_t e = xcfg_set(key, "");
        printf("%s cleared (%s)\n", key, e == ESP_OK ? "saved" : esp_err_to_name(e));
    } else {
        printf("cfg: unknown verb '%s'\n", verb);
    }
    return true;
}
