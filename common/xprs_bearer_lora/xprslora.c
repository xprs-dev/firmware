/* xprslora.c -- see the header. Shaped on xprs_bearer_now, which is the
 * reference for how a radio hides behind xprs_bearer. */

#include "xprslora.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sx1262.h"
#include "xprs.h"
#include "xprsbearer.h"

static const char *TAG = "xprslora";

/* Heap, not BSS, and only once the radio is really there.
 *
 * An xb_t carries the re-air queue (eight 250-byte packets), two 32-slot
 * identifier rings and a peer table -- a few kilobytes that a board with no
 * SX1262 was paying for a bearer it could never start. On the M5Stack that
 * was the difference between an HTTP server that starts and one that
 * answers ESP_ERR_HTTPD_TASK. */
static xb_t *s_lora;
static sx1262_handle_t s_radio;
static SemaphoreHandle_t s_mutex;      /* several tasks air on one radio */
static xprslora_rx_cb_t s_rx_cb;

/* The DIO1 interrupt only raises this flag; every SPI byte moves on the
 * bearer task in lr_drain(). An ISR that touched the bus would collide with
 * whatever transfer the display has in flight. */
static volatile bool s_rx_pending;

static void lr_rx_isr(void *user)
{
    (void)user;
    s_rx_pending = true;
}

/* ── The five function pointers ─────────────────────────────────────────── */

static bool lr_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (!s_radio || len <= 0 || len > XB_WIRE_MAX) return false;
    /* ~400 ms of airtime at SF7: block here, on the bearer task, which is
     * the one task built to wait on radios. The generous timeout is the
     * radio's own TX watchdog, not an expectation. */
    esp_err_t err = sx1262_send(s_radio, (const uint8_t *)wire, (uint8_t)len,
                                3000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
        /* Back to listening either way -- a radio parked in standby after a
         * failed send is deaf and looks exactly like an empty band. */
    }
    sx1262_start_receive(s_radio, lr_rx_isr, NULL);
    return err == ESP_OK;
}

static uint32_t lr_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t lr_random(void) { return esp_random(); }
static void lr_lock(void *ctx)   { (void)ctx; xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void lr_unlock(void *ctx) { (void)ctx; xSemaphoreGive(s_mutex); }

/* On the bearer task, once per tick: fetch what the interrupt announced. */
static void lr_drain(void *ctx)
{
    (void)ctx;
    if (!s_rx_pending || !s_radio || !s_lora) return;
    s_rx_pending = false;

    /* static: this runs only on the one bearer task, and 251 bytes was a
     * meaningful slice of the stack it borrows. */
    static uint8_t buf[XB_WIRE_MAX + 1];
    sx1262_rx_info_t info;
    while (sx1262_get_packet(s_radio, buf, XB_WIRE_MAX, &info) == ESP_OK &&
           info.len > 0) {
        if (!xprs_looks_like(buf, info.len)) {
            ESP_LOGI(TAG, "heard %u bytes that were not XPRS (%d dBm)",
                     (unsigned)info.len, info.rssi);
            break;
        }
        buf[info.len] = 0;
        /* Before the dupe rings swallow it: the one log line that proves a
         * packet crossed on RF rather than on WiFi, with the RSSI only a
         * radio has. Low rate by the nature of the medium. */
        ESP_LOGI(TAG, "RX %u bytes at %d dBm SNR %d: %.48s",
                 (unsigned)info.len, info.rssi, info.snr, (const char *)buf);
        xb_on_wire(s_lora, (const char *)buf, info.len, 0, info.rssi);
        break;   /* one buffer per IRQ; a second packet raises DIO1 again */
    }
}

static void lr_rx_shim(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)peer;
    if (s_rx_cb) s_rx_cb(wire, len, rssi);
}

static const xb_ops_t k_lora_ops = {
    .air = lr_air,
    .now_ms = lr_now_ms,
    .random = lr_random,
    .lock = lr_lock,
    .unlock = lr_unlock,
    .drain = lr_drain,
    .ctx = NULL,
    .name = "lora",
};

/* ── The public bearer ──────────────────────────────────────────────────── */

esp_err_t xprslora_start(const char *callsign, const xprslora_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_radio) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_lora = calloc(1, sizeof *s_lora);
    if (!s_lora) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    sx1262_spi_config_t spi = {
        .mosi_pin = cfg->mosi_pin,
        .miso_pin = cfg->miso_pin,
        .sck_pin = cfg->sck_pin,
        .cs_pin = cfg->cs_pin,
        .rst_pin = cfg->rst_pin,
        .busy_pin = cfg->busy_pin,
        .dio1_pin = cfg->dio1_pin,
    };
    esp_err_t err = sx1262_create(&spi, &s_radio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no radio: %s", esp_err_to_name(err));
        return err;
    }

    /* SF7/125k: ~400 ms for a full packet, and the two ends of a bench test
     * agree on it by construction because both run this line. */
    sx1262_lora_config_t lora = {
        .frequency_hz = cfg->freq_hz ? cfg->freq_hz : 868000000u,
        .sf = SX1262_SF7,
        .bw = SX1262_BW_125,
        .cr = SX1262_CR_4_5,
        .tx_power_dbm = cfg->tx_power_dbm ? cfg->tx_power_dbm : 14,
        .preamble_len = 8,
        .crc_on = true,
        .use_tcxo = cfg->use_tcxo,
        .use_dio2_rf_switch = cfg->use_dio2_rf_switch,
    };
    err = sx1262_init(s_radio, &lora);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(err));
        sx1262_delete(s_radio);
        s_radio = NULL;
        return err;
    }

    xb_init(s_lora, &k_lora_ops, callsign);
    xb_register_ticked(s_lora);
    if (!xb_has_driver())
        ESP_LOGE(TAG, "no bearer task is pumping -- start the LAN bearer "
                      "first, or nothing will ever leave this radio");

    err = sx1262_start_receive(s_radio, lr_rx_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receive mode failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "up: %lu Hz SF7/125k %d dBm as %s",
             (unsigned long)lora.frequency_hz, lora.tx_power_dbm, callsign);
    return ESP_OK;
}

void xprslora_set_rx_cb(xprslora_rx_cb_t cb)
{
    s_rx_cb = cb;
    xb_set_rx_cb(s_lora, cb ? lr_rx_shim : NULL);
}

bool xprslora_send(const char *wire, int len)
{
    return s_lora && xb_send(s_lora, wire, len);
}
void xprslora_offer(const char *wire, int len)
{
    if (s_lora) xb_offer(s_lora, wire, len);
}
bool xprslora_is_active(void)
{
    return s_radio && s_lora && xb_is_active(s_lora);
}

void xprslora_stats(uint32_t *rx, uint32_t *tx, uint32_t *cancelled,
                    uint32_t *dupes)
{
    if (!s_lora) {
        if (rx) *rx = 0;
        if (tx) *tx = 0;
        if (cancelled) *cancelled = 0;
        if (dupes) *dupes = 0;
        return;
    }
    if (rx) *rx = s_lora->rx_count;
    if (tx) *tx = s_lora->tx_count;
    if (cancelled) *cancelled = s_lora->cancelled;
    if (dupes) *dupes = s_lora->dupes;
}
