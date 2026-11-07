/*
 * hotspot_probe -- join every XPRS-* hotspot in reach and check its chat.
 *
 * For each open network whose name starts with "XPRS-": associate, take an
 * address from its DHCP, then fetch from the gateway
 *   /              the chat page (gzipped, so its size is the check)
 *   /api/status    the station's callsign
 *   /api/services  what the page may offer
 * and print one line:
 *   PROBE <ssid> rssi=<dBm> ip=<ours> gw=<gw> page=<code>/<bytes> status=<code> call=<X..> services=<code>
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

#define GOT_IP   BIT0
#define LOST     BIT1
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, GOT_IP);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(s_ev, LOST);
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

/* GET http://<gw><path>; the status, and the body's length (and text when buf). */
static int get(const char *gw, const char *path, body_t *b)
{
    char url[64];
    snprintf(url, sizeof url, "http://%s%s", gw, path);
    esp_http_client_config_t c = {
        .url = url, .event_handler = on_http, .user_data = b, .timeout_ms = 8000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
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

static bool probe(const wifi_ap_record_t *ap)
{
    const char *ssid = (const char *)ap->ssid;
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, ap->ssid, strnlen(ssid, sizeof wc.sta.ssid));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.channel = ap->primary;
    xEventGroupClearBits(s_ev, GOT_IP | LOST);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_ev, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & GOT_IP)) {
        printf("PROBE %s FAIL no address from its DHCP\n", ssid);
        esp_wifi_disconnect();
        return false;
    }
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_sta, &ip);
    char me[20], gw[20];
    snprintf(me, sizeof me, IPSTR, IP2STR(&ip.ip));
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&ip.gw));

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
    bool ok = pc == 200 && page.len > 10000 && sc == 200 && strcmp(call, "-") != 0;
    printf("PROBE %s %s rssi=%d ip=%s gw=%s page=%d/%d status=%d call=%s services=%d/%d\n",
           ssid, ok ? "OK" : "FAIL", ap->rssi, me, gw, pc, page.len, sc, call, vc, sv.len);
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
