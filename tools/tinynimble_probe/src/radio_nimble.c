/* The NimBLE backend, for the A/B. Same air parameters as radio_tiny.c, lifted
 * from common/xprs_bearer_ble/xprsble.c so the only variable is the stack. */
#include "radio.h"

#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/hci_common.h"

static const char *TAG = "radio_nimble";
static radio_rx_fn s_cb;
static uint8_t s_own_addr_type;
static volatile bool s_up, s_adv_configured;

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_EXT_DISC && s_cb)
        s_cb(ev->ext_disc.data, ev->ext_disc.length_data, ev->ext_disc.rssi);
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_up = true;
    ESP_LOGI(TAG, "NimBLE host synced");
}

esp_err_t radio_start(radio_rx_fn cb)
{
    s_cb = cb;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err)); return err; }
    ble_hs_cfg.sync_cb = on_sync;     /* before the host starts, or it races */
    nimble_port_freertos_init(nimble_port_run);

    for (int i = 0; i < 200 && !s_up; i++) vTaskDelay(pdMS_TO_TICKS(10));
    return s_up ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t radio_stop(void)
{
    if (!s_up) return ESP_OK;
    ble_gap_ext_adv_stop(0);
    ble_gap_disc_cancel();
    int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();
    s_up = false; s_adv_configured = false;
    ESP_LOGI(TAG, "NimBLE down (stop rc=%d)", rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool radio_is_up(void) { return s_up; }

esp_err_t radio_advertise(const uint8_t *ad, int len)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(ad, len);
    if (!om) { ESP_LOGW(TAG, "mbuf alloc failed"); return ESP_ERR_NO_MEM; }

    if (!s_adv_configured) {
        struct ble_gap_ext_adv_params p = {0};
        p.connectable = 0; p.scannable = 0; p.legacy_pdu = 0;
        p.own_addr_type = s_own_addr_type;
        p.primary_phy = BLE_HCI_LE_PHY_1M;
        p.secondary_phy = BLE_HCI_LE_PHY_1M;
        p.sid = 0; p.tx_power = 127;
        p.itvl_min = 0x100; p.itvl_max = 0x100;
        int rc = ble_gap_ext_adv_configure(0, &p, NULL, gap_event, NULL);
        if (rc != 0) { os_mbuf_free_chain(om); return ESP_FAIL; }
        s_adv_configured = true;
    } else {
        ble_gap_ext_adv_stop(0);
    }
    if (ble_gap_ext_adv_set_data(0, om) != 0) return ESP_FAIL;
    int rc = ble_gap_ext_adv_start(0, 0, 0);
    return (rc == 0 || rc == BLE_HS_EALREADY) ? ESP_OK : ESP_FAIL;
}

esp_err_t radio_scan_on(void)
{
    struct ble_gap_ext_disc_params uncoded = {
        .itvl = 0x0060, .window = 0x0050, .passive = 1,
    };
    int rc = ble_gap_ext_disc(s_own_addr_type, 0, 0, 0, 0, 0,
                              &uncoded, NULL, gap_event, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t radio_scan_off(void)
{
    return ble_gap_disc_cancel() == 0 ? ESP_OK : ESP_FAIL;
}

const char *radio_name(void) { return "NimBLE"; }

esp_err_t radio_gatt_serve(radio_gatt_rx_fn rx) { (void)rx; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t radio_gatt_send(const uint8_t *d, int n) { (void)d; (void)n; return ESP_ERR_NOT_SUPPORTED; }
void      radio_gatt_pump(void) {}
bool      radio_gatt_connected(void) { return false; }
int       radio_gatt_mtu(void) { return 0; }
