/*
 * hotspot_probe -- join every XPRS-* hotspot in reach and check its chat.
 *
 * For each open network whose name starts with "XPRS-": associate, take an
 * address from its DHCP, then fetch from the gateway
 *   /              the chat page (gzipped, so its size is the check)
 *   /api/status    the station's callsign
 *   /api/services  what the page may offer
 * and print one line:
 *   PROBE <ssid> rssi=<dBm> ip=<ours> gw=<gw> portal=<code> page=<code>/<bytes> status=<code> call=<X..> services=<code>
 * Every request carries the headers an Android WebView sends (PHONE_HDRS),
 * so a server whose header buffer is too small for a phone fails here too.
 * or PROBE <ssid> FAIL <why>. A round ends with PROBE ROUND <n> done <ok>/<seen>,
 * then the next starts two minutes later.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#define GOT_IP   BIT0
#define LOST     BIT1
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;

static int s_reason;   /* of the last disconnect: 201 no AP found, 2 auth expired, 15 handshake, ... */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, GOT_IP);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupSetBits(s_ev, LOST);
    }
}

typedef struct { char *buf; int cap; int len; } body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->buf) {
            int n = e->data_len;
            if (b->len + n > b->cap - 1) n = b->cap - 1 - b->len;
            if (n > 0) { memcpy(b->buf + b->len, e->data, n); b->len += n; b->buf[b->len] = 0; }
        } else {
            b->len += e->data_len;
        }
    }
    return ESP_OK;
}

/* The headers an Android captive-portal WebView sends, about 760 bytes with
 * the Host line. A server built with the 512-byte default answers them
 * 431 "Header fields are too long" while a bare client passes, which is
 * how the T-Dongle shipped with a hotspot no phone could open. */
static const char *const PHONE_HDRS[][2] = {
    { "User-Agent", "Mozilla/5.0 (Linux; Android 14; Pixel 7 Build/UP1A.231005.007; wv) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
                    "Chrome/130.0.6723.86 Mobile Safari/537.36" },
    { "Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
                "image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7" },
    { "Accept-Language", "en-US,en;q=0.9,pt-PT;q=0.8,pt;q=0.7" },
    { "Accept-Encoding", "gzip, deflate" },
    { "Upgrade-Insecure-Requests", "1" },
    { "sec-ch-ua", "\"Chromium\";v=\"130\", \"Android WebView\";v=\"130\", \"Not?A_Brand\";v=\"99\"" },
    { "sec-ch-ua-mobile", "?1" },
    { "sec-ch-ua-platform", "\"Android\"" },
    { "Sec-Fetch-Site", "none" },
    { "Sec-Fetch-Mode", "navigate" },
    { "Sec-Fetch-User", "?1" },
    { "Sec-Fetch-Dest", "document" },
    { "X-Requested-With", "com.android.captiveportallogin" },
    { "Cookie", "xprs_nsec=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" },
    { "Connection", "keep-alive" },
};

/* GET http://<gw><path> with a phone's headers; the status, and the body's
 * length (and text when buf). */
static int get(const char *gw, const char *path, body_t *b)
{
    char url[128];
    snprintf(url, sizeof url, "http://%s%s", gw, path);
    esp_http_client_config_t c = {
        .url = url, .event_handler = on_http, .user_data = b, .timeout_ms = 8000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    for (size_t i = 0; i < sizeof PHONE_HDRS / sizeof PHONE_HDRS[0]; i++)
        esp_http_client_set_header(h, PHONE_HDRS[i][0], PHONE_HDRS[i][1]);
    int code = -1;
    if (esp_http_client_perform(h) == ESP_OK) code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    return code;
}

/* The value of "key":"..." in a small JSON body, or "-". */
static void field(const char *json, const char *key, char *out, int n)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) { snprintf(out, n, "-"); return; }
    p += strlen(pat);
    int i = 0;
    while (p[i] && p[i] != '"' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

/* A WebView opens the page's first requests together, not one after the
 * other: history, services, alerts, blog, rooms and session in flight at
 * once against a server with one task and seven sockets. Three tasks fire
 * two each and the slowest decides; the page's own timeout is 8 s. */
typedef struct { const char *gw; const char *url[2]; int code[2]; int ms[2]; TaskHandle_t waiter; } burst_t;
static void burst_task(void *arg)
{
    burst_t *b = arg;
    for (int i = 0; i < 2; i++) {
        body_t body = { 0 };
        int64_t t0 = esp_timer_get_time();
        b->code[i] = get(b->gw, b->url[i], &body);
        b->ms[i] = (int)((esp_timer_get_time() - t0) / 1000);
    }
    xTaskNotifyGive(b->waiter);
    vTaskDelete(NULL);
}
static bool burst(const char *ssid, const char *gw)
{
    static burst_t b[3];
    static const char *const urls[6] = {
        "/api/xprs/history?kind=message,reaction,status&limit=40", "/api/services",
        "/api/xprs/history?kind=warning,info&limit=100", "/api/xprs/history?kind=blog&limit=200",
        "/api/rooms", "/api/session",
    };
    for (int t = 0; t < 3; t++) {
        b[t].gw = gw; b[t].url[0] = urls[2 * t]; b[t].url[1] = urls[2 * t + 1];
        b[t].waiter = xTaskGetCurrentTaskHandle();
        xTaskCreate(burst_task, "burst", 4096, &b[t], 5, NULL);
    }
    for (int t = 0; t < 3; t++) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    bool ok = true;
    int worst = 0;
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 2; i++) {
            if (b[t].code[i] != 200 || b[t].ms[i] > 8000) ok = false;
            if (b[t].ms[i] > worst) worst = b[t].ms[i];
            printf("PROBE %s BURST %s code=%d ms=%d\n", ssid, b[t].url[i], b[t].code[i], b[t].ms[i]);
        }
    printf("PROBE %s BURST %s worst=%d ms\n", ssid, ok ? "OK" : "FAIL", worst);
    return ok;
}

static bool probe(const wifi_ap_record_t *ap)
{
    const char *ssid = (const char *)ap->ssid;
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, ap->ssid, strnlen(ssid, sizeof wc.sta.ssid));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.channel = ap->primary;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    /* A phone's supplicant tries an association several times before it
     * reports a network unusable; one try in five failed here at the
     * auth/assoc step with the AP never seeing a join, so this does too.
     * The count is printed: a hotspot that needs three tries is news. */
    int tries = 0;
    EventBits_t bits = 0;
    while (tries < 3 && !(bits & GOT_IP)) {
        xEventGroupClearBits(s_ev, GOT_IP | LOST);
        esp_wifi_connect();
        tries++;
        bits = xEventGroupWaitBits(s_ev, GOT_IP | LOST, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (!(bits & GOT_IP)) {
            printf("PROBE %s try %d: %s (reason %d)\n", ssid, tries,
                   (bits & LOST) ? "disconnected" : "no address in 15 s", (bits & LOST) ? s_reason : 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!(bits & GOT_IP)) {
        printf("PROBE %s FAIL no address from its DHCP after %d tries\n", ssid, tries);
        esp_wifi_disconnect();
        return false;
    }
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_sta, &ip);
    char me[20], gw[20];
    snprintf(me, sizeof me, IPSTR, IP2STR(&ip.ip));
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&ip.gw));

    body_t portal = { 0 };
    int cc = get(gw, "/generate_204", &portal);   /* Android's captive check */
    body_t page = { 0 };
    int pc = get(gw, "/", &page);
    static char js[1024];
    body_t st = { js, sizeof js, 0 };
    js[0] = 0;
    int sc = get(gw, "/api/status", &st);
    char call[16];
    field(js, "callsign", call, sizeof call);
    body_t sv = { 0 };
    int vc = get(gw, "/api/services", &sv);
    /* What the chat page itself asks for in its first seconds, timed: the
     * page shows "connecting" until the first of these answers within 4 s. */
    static const char *const PAGE_URLS[] = {
        "/api/xprs/history?kind=message,reaction,status&limit=200",
        "/api/rooms",
        "/api/xprs/history?kind=warning,info&limit=100",
        "/api/xprs/history?kind=blog&limit=200",
        "/api/session",
        "/api/xprs/history?kind=message,reaction,status&limit=40",
    };
    for (size_t i = 0; i < sizeof PAGE_URLS / sizeof PAGE_URLS[0]; i++) {
        body_t b = { 0 };
        int64_t t0 = esp_timer_get_time();
        int c = get(gw, PAGE_URLS[i], &b);
        printf("PROBE %s PAGE %s code=%d len=%d ms=%lld\n", ssid, PAGE_URLS[i], c, b.len,
               (long long)((esp_timer_get_time() - t0) / 1000));
    }
    bool bok = burst(ssid, gw);
    bool ok = bok && cc == 200 && pc == 200 && page.len > 10000 && sc == 200 && strcmp(call, "-") != 0;
    printf("PROBE %s %s rssi=%d tries=%d ip=%s gw=%s portal=%d page=%d/%d status=%d call=%s services=%d/%d\n",
           ssid, ok ? "OK" : "FAIL", ap->rssi, tries, me, gw, cc, pc, page.len, sc, call, vc, sv.len);
    esp_wifi_disconnect();
    xEventGroupWaitBits(s_ev, LOST, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    return ok;
}

void app_main(void)
{
    if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ev = xEventGroupCreate();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    printf("PROBE hotspot probe up\n");

    for (int round = 1;; round++) {
        wifi_scan_config_t sc = { .show_hidden = false };
        esp_wifi_scan_start(&sc, true);
        uint16_t n = 20;
        static wifi_ap_record_t aps[20];
        esp_wifi_scan_get_ap_records(&n, aps);
        int seen = 0, ok = 0;
        char done[8][33];
        for (int i = 0; i < n; i++) {
            const char *s = (const char *)aps[i].ssid;
            if (strncmp(s, "XPRS-", 5) != 0 || aps[i].authmode != WIFI_AUTH_OPEN) continue;
            bool dup = false;   /* one try per name, at its strongest */
            for (int k = 0; k < seen && k < 8; k++) if (!strcmp(done[k], s)) dup = true;
            if (dup || seen >= 8) continue;
            memcpy(done[seen], s, strnlen(s, 32)); done[seen][strnlen(s, 32)] = 0; seen++;
            if (probe(&aps[i])) ok++;
        }
        printf("PROBE ROUND %d done %d/%d\n", round, ok, seen);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(120000));
    }
}
