/**
 * @file xprs_hotspot.c
 * @brief AP + captive portal + the chat page (see xprs_hotspot.h).
 */

#include "xprs_hotspot.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "dns_server.h"

static const char *TAG = "xprs_hotspot";

extern const unsigned char XPRS_CHAT_PAGE_GZ[];
extern const size_t XPRS_CHAT_PAGE_GZ_LEN;

/* The page, gzipped in flash and chunked out -- 38 KB of HTML, JS and a
 * base64 WOFF2 compresses to 18 KB, and it never has to be decompressed
 * here: the browser does that. Chunked because one httpd_resp_send would
 * want a contiguous buffer the heap should not be asked for.
 *
 * Content-Encoding: gzip is sent unconditionally rather than switched on
 * Accept-Encoding. Every captive-portal WebView is Chrome or WebKit and
 * every one of them accepts gzip; the only clients that would not are the
 * bare connectivity probes, which discard the body anyway. */
static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    for (size_t off = 0; off < XPRS_CHAT_PAGE_GZ_LEN; off += 1024) {
        size_t n = XPRS_CHAT_PAGE_GZ_LEN - off;
        if (n > 1024) n = 1024;
        if (httpd_resp_send_chunk(req, (const char *)XPRS_CHAT_PAGE_GZ + off,
                                  n) != ESP_OK)
            break;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Unknown URI: background apps (the phone's whole app list, once captive DNS
 * answers everything with our address) get a redirect and a CLOSED socket --
 * the old firmware learned that keeping those alive exhausts the pool. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;   /* tells httpd to close the socket */
}

esp_err_t xprs_hotspot_serve_page(httpd_handle_t server)
{
    static bool registered;
    if (!server) return ESP_ERR_INVALID_ARG;
    if (registered) return ESP_OK;
    registered = true;

    static const httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET,
                                        .handler = h_page };
    /* Captive-portal detection gets the PAGE, not a redirect: that is what
     * makes the sign-in popup BE the chat (the old firmware's discovery). */
    static const httpd_uri_t u_204 = { .uri = "/generate_204",
                                       .method = HTTP_GET, .handler = h_page };
    static const httpd_uri_t u_apple = { .uri = "/hotspot-detect.html",
                                         .method = HTTP_GET,
                                         .handler = h_page };
    httpd_register_uri_handler(server, &u_root);
    httpd_register_uri_handler(server, &u_204);
    httpd_register_uri_handler(server, &u_apple);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, h_404);
    return ESP_OK;
}

esp_err_t xprs_hotspot_start(const char *ssid, httpd_handle_t server)
{
    if (!ssid || !ssid[0] || !server) return ESP_ERR_INVALID_ARG;

    /* The AP netif + DHCP server. The driver is already up (STA or NULL
     * mode); flip to APSTA WITHOUT stopping it, or the STA drops. */
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_FAIL;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    esp_err_t ret = esp_wifi_set_mode(
        mode == WIFI_MODE_STA ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof wc.ap.ssid, "%s", ssid);
    wc.ap.ssid_len = strlen((const char *)wc.ap.ssid);
    wc.ap.authmode = WIFI_AUTH_OPEN;      /* a walk-up chat has no password */
    wc.ap.max_connection = 4;
    /* With a STA up the hardware forces the AP onto the STA's channel; this
     * value only matters when the AP stands alone, and then it should be
     * the ESP-NOW fallback channel so the radio side still meets us. */
    wc.ap.channel = 1;
    ret = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AP config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* The mode switch can leave the AP's DHCP server stopped (the old
     * firmware's wifi_bsp restarted it after every switch; same here). */
    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_start(ap);

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
        dns_server_start(ip.ip.addr);
        ESP_LOGI(TAG, "hotspot \"%s\" up, captive at " IPSTR, ssid,
                 IP2STR(&ip.ip));
    }

    return xprs_hotspot_serve_page(server);
}
