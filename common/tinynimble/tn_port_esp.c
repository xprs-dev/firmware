/* The only part of tinynimble that knows about ESP-IDF: bring the controller
 * up, hand it HCI packets, and take its events back. See tinynimble.h.
 *
 * There is no host task and there are no mbuf pools, which is the entire point
 * -- those are what NimBLE spends its heap on. What is left is one semaphore,
 * one 64-byte command buffer and one 260-byte staging buffer for the advert.
 *
 * THE RECEIVE CALLBACK RUNS IN THE CONTROLLER'S CONTEXT.
 * Whatever the caller does in tn_rx_cb must be free: parse, identify,
 * deduplicate, and park the work for another task. This is the same rule
 * docs/esp32.md states for every receive path in this firmware, and here it is
 * stricter than usual because the context belongs to the link layer. */

#include "tinynimble.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_bt.h"
#include "esp_log.h"

static const char *TAG = "tinynimble";

/* One command in flight at a time. Bring-up is a short sequence of
 * command/response pairs and nothing here is on a hot path, so a single
 * slot costs one semaphore and removes every ordering question. */
static SemaphoreHandle_t s_cmd_done;
static uint16_t          s_cmd_opcode;
static volatile uint8_t  s_cmd_status;

static SemaphoreHandle_t s_can_send;

static tn_report_cb_t s_rx_cb;
static void          *s_rx_ctx;
static bool           s_up;

/* ── VHCI callbacks, both in controller context ─────────────────────────── */

static void vhci_send_available(void)
{
    if (s_can_send) xSemaphoreGive(s_can_send);
}

static int vhci_recv(uint8_t *data, uint16_t len)
{
    /* A command result unblocks tn_send_cmd(); an advertising report goes
     * straight to the caller. Everything else the controller says is ignored,
     * which is the whole event policy. */
    uint8_t status;
    if (tn_hci_cmd_result(data, len, s_cmd_opcode, &status)) {
        s_cmd_status = status;
        if (s_cmd_done) xSemaphoreGive(s_cmd_done);
        return 0;
    }
    tn_hci_feed_evt(data, len, s_rx_cb, s_rx_ctx);
    return 0;
}

static const esp_vhci_host_callback_t s_vhci_cb = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv           = vhci_recv,
};

/* ── one command, and wait for the controller to answer ─────────────────── */

static esp_err_t tn_send_cmd(const uint8_t *pkt, int len, uint16_t opcode)
{
    if (len <= 0) return ESP_ERR_INVALID_ARG;

    /* The controller tells us when it can take another packet; asking before
     * it is ready silently drops the command. */
    if (!esp_vhci_host_check_send_available()) {
        if (xSemaphoreTake(s_can_send, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "controller never became ready for opcode 0x%04x", opcode);
            return ESP_ERR_TIMEOUT;
        }
    }

    s_cmd_opcode = opcode;
    s_cmd_status = 0xFF;
    xSemaphoreTake(s_cmd_done, 0);          /* clear any stale give */

    esp_vhci_host_send_packet((uint8_t *)pkt, (uint16_t)len);

    if (xSemaphoreTake(s_cmd_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "opcode 0x%04x got no answer in 1 s", opcode);
        return ESP_ERR_TIMEOUT;
    }
    if (s_cmd_status != 0x00) {
        /* Say the opcode AND the status. A bare "BLE failed" is what sends the
         * next person to the antenna. */
        ESP_LOGE(TAG, "opcode 0x%04x refused, HCI status 0x%02x",
                 opcode, s_cmd_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

esp_err_t tn_start(void)
{
    if (s_up) return ESP_OK;

    if (!s_cmd_done) s_cmd_done = xSemaphoreCreateBinary();
    if (!s_can_send) s_can_send = xSemaphoreCreateBinary();
    if (!s_cmd_done || !s_can_send) {
        ESP_LOGE(TAG, "semaphores refused -- not starting");
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        /* The classic presentation of this is BLE_INIT: Malloc failed, and the
         * cause is almost always that WiFi went first. docs/esp32.md and the
         * T-Deck config both record it: start BLE BEFORE WiFi. */
        ESP_LOGE(TAG, "controller init failed: %s -- was WiFi started first?",
                 esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return err;
    }
    err = esp_vhci_host_register_callback(&s_vhci_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vhci register failed: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return err;
    }

    s_up = true;   /* commands may be sent from here on */

    /* The bring-up sequence. Skipping the two masks costs an afternoon: the
     * controller accepts scan parameters and a scan enable, reports no error,
     * and delivers nothing, because LE Meta and Extended Advertising Report
     * are both masked off by default. */
    uint8_t buf[16];
    int n;

    n = tn_hci_reset(buf, sizeof buf);
    if (n > 0 && tn_send_cmd(buf, n, TN_OP_RESET) != ESP_OK) goto fail;

    n = tn_hci_set_event_mask(buf, sizeof buf,
                              TN_EVENT_MASK_DEFAULT | TN_EVENT_MASK_LE_META);
    if (n > 0 && tn_send_cmd(buf, n, TN_OP_SET_EVENT_MASK) != ESP_OK) goto fail;

    n = tn_hci_le_set_event_mask(buf, sizeof buf,
                                 TN_LE_EVENT_MASK_DEFAULT | TN_LE_EVENT_MASK_EXT_ADV);
    if (n > 0 && tn_send_cmd(buf, n, TN_OP_LE_SET_EVENT_MASK) != ESP_OK) goto fail;

    ESP_LOGI(TAG, "controller up, HCI direct (no host stack)");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "bring-up sequence failed -- taking the controller back down");
    s_up = false;
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    return ESP_FAIL;
}

esp_err_t tn_stop(void)
{
    if (!s_up) return ESP_OK;

    /* Stop advertising and scanning before dropping the controller, so it is
     * not left mid-operation. Failures are logged, not fatal: the point of
     * this call is to give the radio back and it must not be blockable.
     *
     * This path is not decoration. With the BLE controller running, a WiFi
     * station that is NOT associated receives nothing -- measured, 1 of 36
     * frames, and cancelling the scan alone does not give it back
     * (docs/espnow.md). The ESP-NOW working-channel move depends on a full
     * teardown, so tn_stop() has to be as complete as
     * nimble_port_deinit() was. */
    uint8_t buf[16];
    int n = tn_hci_ext_adv_enable(buf, sizeof buf, 0, false);
    if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);
    n = tn_hci_ext_scan_enable(buf, sizeof buf, false, false);
    if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_SCAN_ENABLE);

    esp_err_t e1 = esp_bt_controller_disable();
    esp_err_t e2 = esp_bt_controller_deinit();
    s_up = false;
    s_rx_cb = NULL;
    ESP_LOGI(TAG, "controller down (disable %s, deinit %s)",
             esp_err_to_name(e1), esp_err_to_name(e2));
    return (e1 == ESP_OK && e2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

bool tn_is_up(void) { return s_up; }

/* ── advertising ────────────────────────────────────────────────────────── */

static tn_adv_cfg_t s_adv;
static bool         s_adv_configured;
static uint8_t      s_rand_addr[6];

esp_err_t tn_adv_configure(const tn_adv_cfg_t *cfg)
{
    if (!s_up || !cfg) return ESP_ERR_INVALID_STATE;
    uint8_t buf[40];
    int n = tn_hci_ext_adv_params(buf, sizeof buf, cfg);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = tn_send_cmd(buf, n, TN_OP_EXT_ADV_PARAMS);
    if (err != ESP_OK) return err;

    /* The set's OWN address. An extended advertising set does not inherit the
     * one 0x2005 sets -- skip this and the controller takes the parameters,
     * takes the data, then refuses to enable with 0x12, blaming the enable. */
    if (cfg->own_addr_type == 0x01) {
        n = tn_hci_adv_set_random_addr(buf, sizeof buf, cfg->handle, s_rand_addr);
        if (n < 0) return ESP_ERR_INVALID_ARG;
        err = tn_send_cmd(buf, n, TN_OP_ADV_SET_RAND_ADDR);
        if (err != ESP_OK) return err;
    }

    s_adv = *cfg; s_adv_configured = true;
    return ESP_OK;
}

esp_err_t tn_adv_set_data(const uint8_t *ad, size_t len)
{
    if (!s_up || !s_adv_configured) return ESP_ERR_INVALID_STATE;
    if (len > TN_ADV_DATA_MAX) return ESP_ERR_INVALID_SIZE;

    /* stop -> set -> start, and NOT a reconfigure. Recreating the advertising
     * set makes the controller rotate its random address, which fragments
     * every peer's address book (see common/geogram_xprsble/xprsble.c). */
    static uint8_t buf[4 + 4 + TN_ADV_DATA_MAX];
    int n;

    n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, false);
    if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);

    n = tn_hci_ext_adv_data(buf, sizeof buf, s_adv.handle, ad, len);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = tn_send_cmd(buf, n, TN_OP_EXT_ADV_DATA);
    if (err != ESP_OK) return err;

    n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, true);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    return tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);
}

esp_err_t tn_adv_stop(void)
{
    if (!s_up) return ESP_OK;
    uint8_t buf[16];
    int n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, false);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    return tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);
}

/* ── scanning ───────────────────────────────────────────────────────────── */

esp_err_t tn_scan_start(const tn_scan_cfg_t *cfg, tn_report_cb_t cb, void *ctx)
{
    if (!s_up || !cfg) return ESP_ERR_INVALID_STATE;
    uint8_t buf[24];

    int n = tn_hci_ext_scan_params(buf, sizeof buf, cfg);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = tn_send_cmd(buf, n, TN_OP_EXT_SCAN_PARAMS);
    if (err != ESP_OK) return err;

    s_rx_cb  = cb;      /* set before enabling, or the first report is lost */
    s_rx_ctx = ctx;

    n = tn_hci_ext_scan_enable(buf, sizeof buf, true, false);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    return tn_send_cmd(buf, n, TN_OP_EXT_SCAN_ENABLE);
}

esp_err_t tn_scan_stop(void)
{
    if (!s_up) return ESP_OK;
    uint8_t buf[16];
    int n = tn_hci_ext_scan_enable(buf, sizeof buf, false, false);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    return tn_send_cmd(buf, n, TN_OP_EXT_SCAN_ENABLE);
}

esp_err_t tn_set_random_addr(const uint8_t addr[6])
{
    if (!s_up || !addr) return ESP_ERR_INVALID_STATE;
    uint8_t buf[16];
    int n = tn_hci_set_random_addr(buf, sizeof buf, addr);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    memcpy(s_rand_addr, addr, 6);   /* remembered for the advertising set */
    return tn_send_cmd(buf, n, TN_OP_SET_RANDOM_ADDR);
}
