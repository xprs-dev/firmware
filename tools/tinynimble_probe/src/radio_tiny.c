/* The tinynimble backend: six commands, one event, no host stack. */
#include "radio.h"
#include "tinynimble.h"

#include <string.h>
#include "esp_mac.h"
#include "esp_log.h"

static radio_rx_fn s_cb;
static tn_adv_cfg_t s_adv_cfg;

/* CONTROLLER context -- see tinynimble.h. Hand the bytes straight up. */
static void on_report(const tn_adv_report_t *r, void *ctx)
{
    (void)ctx;
    if (s_cb) s_cb(r->data, r->data_len, r->rssi);
}

esp_err_t radio_start(radio_rx_fn cb)
{
    s_cb = cb;
    esp_err_t err = tn_start();
    if (err != ESP_OK) return err;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    mac[0] |= 0xC0;                       /* static random */
    tn_set_random_addr(mac);

    tn_adv_cfg_t adv = {
        .handle = 0, .props = 0,
        .itvl_min = 0x100, .itvl_max = 0x100,
        .chan_map = 0x07, .own_addr_type = 0x01,
        .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M,
        .sid = 0,
    };
    s_adv_cfg = adv;
    return tn_adv_configure(&adv);
}

/* ── the mesh channel ───────────────────────────────────────────────────── */

static radio_gatt_rx_fn s_gatt_rx;
static volatile int s_links;

static void on_connected(void *ctx, uint16_t conn, bool central)
{
    (void)ctx; (void)central;
    s_links++;
    printf("  link up 0x%04x\n", conn);
}
static void on_disconnected(void *ctx, uint16_t conn, uint8_t reason)
{
    (void)ctx;
    printf("  link down 0x%04x, reason 0x%02x\n", conn, reason);
}
static void on_rx(void *ctx, const uint8_t *d, int n)
{
    (void)ctx;
    if (s_gatt_rx) s_gatt_rx(d, n);
}

esp_err_t radio_gatt_serve(radio_gatt_rx_fn rx)
{
    s_gatt_rx = rx;
    /* The ONE set becomes connectable: the beacon is the presence advert,
     * which is what a chip with a single set has to do (docs/ble5-gatt.md).
     * Reconfiguring the parameters keeps the set's random address because
     * the address is set separately and not touched here. */
    s_adv_cfg.props = TN_ADV_PROP_CONNECTABLE;
    esp_err_t err = tn_adv_configure(&s_adv_cfg);
    if (err != ESP_OK) return err;
    static const tn_gatt_cb_t cb = {
        .connected = on_connected, .disconnected = on_disconnected, .rx = on_rx,
    };
    return tn_gatt_serve(&cb);
}

esp_err_t radio_gatt_send(const uint8_t *d, int n) { return tn_gatt_send(d, n); }
void      radio_gatt_pump(void)      { tn_gatt_pump(); }
bool      radio_gatt_connected(void) { return tn_gatt_connected(); }
int       radio_gatt_mtu(void)       { return tn_gatt_mtu(); }

esp_err_t radio_stop(void)   { return tn_stop(); }
bool      radio_is_up(void)  { return tn_is_up(); }
esp_err_t radio_advertise(const uint8_t *ad, int len)
                             { return tn_adv_set_data(ad, (size_t)len); }

esp_err_t radio_scan_on(void)
{
    tn_scan_cfg_t scan = {
        .own_addr_type = 0x01, .passive = 1,
        .itvl = 0x0060, .window = 0x0050, .phy = TN_PHY_1M,
    };
    return tn_scan_start(&scan, on_report, NULL);
}

esp_err_t radio_scan_off(void) { return tn_scan_stop(); }

const char *radio_name(void) { return "tinynimble"; }
