/*
 * The XPRS-WiFi relay: the ESP32-C3 as a bent pipe on the long-range bearer.
 *
 * The full station (common/xprs_app) does not fit beside xwifi on this chip:
 * the RF sample bank claims 128 KB of the one SRAM at 0x3fca0000, which caps
 * static data at ~55 KB, and the station's is ~100 KB however it is placed.
 * So the relay build (-DXPRS_XWIFI_RELAY, env c3xwifi) is this file instead:
 * the real bearer with its dedup rings, jitter and §13.2.1 cancel
 * (xprs_bearer through xprs_bearer_wifi8), a §10.6.1 observation beacon, and
 * §13.1 digipeating. Everything it hears and airs is on the serial console,
 * which is how the bench reads it.
 *
 * The normal c3mini env is untouched: ESP-NOW era, full station, no xwifi.
 */
#ifdef XPRS_XWIFI_RELAY

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "xprswifi8.h"

static const char *TAG = "xwrelay";

static char s_call[8];

/* The station program's MAC fallback (xprs_app.c derive_callsign), verbatim:
 * X5 and four base-32 characters of the WiFi MAC's low bytes. */
static void derive_callsign(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static const char *b32 = "ACDEFGHJKLMNPQRSTUVWXYZ23456789";  /* no B/I/O/1 */
    uint32_t v = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    s_call[0] = 'X'; s_call[1] = '5';
    for (int i = 0; i < 4; i++) { s_call[2 + i] = b32[v % 30]; v /= 30; }
    s_call[6] = 0;
}

/* A minimal §10.6.1 observation: unsigned and clockless, which a hub would
 * ignore but a bench station logs and counts -- enough to prove the air. */
static int beacon(char *out, int cap)
{
    return snprintf(out, (size_t)cap,
                    "t:observation f:%s link:wifi8 relay:1 uptime:%lum",
                    s_call, (unsigned long)(esp_timer_get_time() / 60000000));
}

static void on_rx(const char *wire, int len, int snr_db)
{
    ESP_LOGI(TAG, "heard %3dB at ~%d dB  %.100s", len, snr_db, wire);
    /* §13.1: repeat it on the medium it came from, for stations past the
     * sender's reach. The bearer's rings keep it from looping. */
    xprswifi8_digipeat(wire, len);
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    derive_callsign();
    ESP_LOGI(TAG, "XPRS-WiFi relay %s", s_call);

    /* Full-time radio (nothing else wants it on this build), both looks: on
     * the C3 the second look's staging and window live in the RF bank's lower
     * half, so the extra ~2.5 dB of sensitivity costs no heap here. */
    xprswifi8_cfg_t wc = XPRSWIFI8_CFG_DEFAULT;
    wc.slot_ms = 0;
    wc.looks = 2;
    ESP_ERROR_CHECK(xprswifi8_start(s_call, &wc));
    xprswifi8_set_rx_cb(on_rx);
    xprswifi8_set_trace(true);
    xprswifi8_set_beacon(beacon, 30, 5);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        uint32_t rx, tx, can, drop, iss, done, fail;
        xprswifi8_stats(&rx, &tx, &can, &drop);
        xprswifi8_tx_stats(&iss, &done, &fail);
        ESP_LOGI(TAG, "alive %s rx=%lu tx=%lu cancel=%lu drop=%lu "
                      "issued=%lu done=%lu fail=%lu heap=%u/%u",
                 s_call, (unsigned long)rx, (unsigned long)tx,
                 (unsigned long)can, (unsigned long)drop,
                 (unsigned long)iss, (unsigned long)done, (unsigned long)fail,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

#endif /* XPRS_XWIFI_RELAY */
