/*
 * The HTTPS memory probe (docs/plan-firmware-https.md): can the C3 serve
 * HTTPS beside the station? A test build only (env c3https): an
 * esp_https_server on 443 with a self-signed ECDSA P-256 certificate, the
 * real chat page at /, and /heap reporting what the handshakes and pages
 * cost. A task samples the internal heap every 10 ms and keeps the lowest
 * free size and the smallest largest-block seen since /heap?reset.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "https_probe_cert.h"

static const char *TAG = "https_probe";
extern const unsigned char XPRS_CHAT_PAGE_GZ[];
extern const size_t XPRS_CHAT_PAGE_GZ_LEN;

static volatile size_t s_min_free, s_min_large, s_base_free, s_base_large;
static volatile uint32_t s_pages, s_heaps;
static TaskHandle_t s_srv_task;

static void reset_marks(void)
{
    s_base_free = s_min_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_base_large = s_min_large = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

static void sampler(void *arg)
{
    (void)arg;
    for (;;) {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (f < s_min_free) s_min_free = f;
        if (l < s_min_large) s_min_large = l;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t h_page(httpd_req_t *req)
{
    if (!s_srv_task) s_srv_task = xTaskGetCurrentTaskHandle();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    s_pages++;
    return httpd_resp_send(req, (const char *)XPRS_CHAT_PAGE_GZ, (ssize_t)XPRS_CHAT_PAGE_GZ_LEN);
}

static esp_err_t h_heap(httpd_req_t *req)
{
    if (!s_srv_task) s_srv_task = xTaskGetCurrentTaskHandle();
    char q[16] = "";
    bool reset = httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "reset");
    char out[320];
    int n = snprintf(out, sizeof out,
        "{\"free\":%u,\"largest\":%u,\"min_free\":%u,\"min_largest\":%u,"
        "\"base_free\":%u,\"base_largest\":%u,\"boot_min\":%u,\"pages\":%u,"
        "\"stack_left\":%u}",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)s_min_free, (unsigned)s_min_large,
        (unsigned)s_base_free, (unsigned)s_base_large,
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)s_pages,
        (unsigned)(s_srv_task ? uxTaskGetStackHighWaterMark(s_srv_task) : 0));
    if (reset) reset_marks();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, n);
}

static void probe_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(25000));      /* after WiFi, the API and the hotspot */
    reset_marks();
    ESP_LOGW(TAG, "before the HTTPS server: free %u, largest %u",
             (unsigned)s_base_free, (unsigned)s_base_large);
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.servercert = (const uint8_t *)k_probe_cert;
    conf.servercert_len = sizeof k_probe_cert;
    conf.prvtkey_pem = (const uint8_t *)k_probe_key;
    conf.prvtkey_len = sizeof k_probe_key;
    conf.session_tickets = true;
    conf.httpd.max_open_sockets = 1;
    conf.httpd.backlog_conn = 4;
    conf.httpd.lru_purge_enable = false;   /* queued in the backlog, not dropped */
    conf.httpd.recv_wait_timeout = 5;
    conf.httpd.send_wait_timeout = 5;
    conf.httpd.ctrl_port = 32790;
#ifdef XPRS_HTTPS_STACK
    conf.httpd.stack_size = XPRS_HTTPS_STACK;
#endif
    httpd_handle_t h = NULL;
    esp_err_t e = httpd_ssl_start(&h, &conf);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start: %s (free %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelete(NULL);
        return;
    }
    static const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = h_page };
    static const httpd_uri_t heap = { .uri = "/heap", .method = HTTP_GET, .handler = h_heap };
    httpd_register_uri_handler(h, &page);
    httpd_register_uri_handler(h, &heap);
    ESP_LOGW(TAG, "HTTPS up on 443: free %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    reset_marks();
    xTaskCreate(sampler, "hsample", 2048, NULL, 2, NULL);
    vTaskDelete(NULL);
}

void https_probe_start(void)
{
    xTaskCreate(probe_task, "hprobe", 4096, NULL, 3, NULL);
}
