/* XPRS over BLE5 extended advertising — see xprsble.h and docs/ble5.md.
 *
 * The radio and nothing else: no queue, no dedup, no relay policy. Those are
 * the caller's, the same division geogram_xprsnow keeps with
 * geogram_xprsbearer. What lives here is the controller, the host, one
 * extended-advertising instance and one extended scan.
 *
 * Lifted from the T-Dongle's main.c, which is where this ran first and where it
 * was proved; the dongle keeps its own copy until it is migrated separately.
 */

#include "xprsble.h"

#if !CONFIG_BT_ENABLED
/*
 * NO BLUETOOTH ON THIS BOARD -- the whole bearer compiles to nothing.
 *
 * xprs_app already treats BLE as optional at RUNTIME: xprs_app.h's board
 * descriptor carries `bool ble`, and "leaving the flag false is how that is
 * said, and the M5Stack says it". But the include and the REQUIRES are
 * unconditional, and with CONFIG_BT_ENABLED=n ESP-IDF's `bt` component
 * registers with an EMPTY include_dirs -- so neither nimble/nimble_port.h nor
 * esp_bt.h resolves and the build dies before reaching any runtime flag.
 *
 * The guard belongs here rather than in every caller because xprsble.h pulls in
 * only <stdint.h>, <stdbool.h> and "esp_err.h", so it is safe to include
 * anywhere. Callers need no #ifdef: xprsble_is_active() answering false is
 * already the "this board cannot carry this bearer" path they all take.
 *
 * The M5Stack is not merely unconfigured, it is incapable: the original ESP32
 * defines SOC_BLE_SUPPORTED but not SOC_BLE_50_SUPPORTED, so it has only legacy
 * 31-byte advertising, and an XPRS frame does not fit in 31 bytes.
 */
esp_err_t xprsble_start(const char *callsign)
{
    (void)callsign;
    return ESP_ERR_NOT_SUPPORTED;
}

bool xprsble_is_active(void) { return false; }
void xprsble_set_rx_cb(xprsble_rx_cb_t cb) { (void)cb; }

bool xprsble_send(const char *wire, int len)
{
    (void)wire; (void)len;
    return false;
}

bool xprsble_send_sub(const uint8_t *payload, int len, uint8_t subtype)
{
    (void)payload; (void)len; (void)subtype;
    return false;
}

uint32_t xprsble_scan_results(void) { return 0; }

/* -1 is what the live implementation returns before the first frame is heard:
 * "nothing to report", not "silent for zero seconds". */
int xprsble_silent_for(void) { return -1; }

#else  /* CONFIG_BT_ENABLED */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"

#if CONFIG_XPRSBLE_BACKEND_TINYNIMBLE
/* Same radio, ~21 KB less flash and ~22 KB less heap, because the NimBLE host
 * is not linked at all -- see common/tinynimble/README.md for the measured A/B
 * and the two-way interop against NimBLE on the air. */
#include "tinynimble.h"
#else
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#endif

static const char *TAG = "xprsble";

/* docs/ble5.md §2: company 0xFFFF, marker 0x3E, then the subtype. */
#define COMPANY_LO 0xFF
#define COMPANY_HI 0xFF
#define MARKER     0x3E

static char            s_call[16];
static uint8_t         s_own_addr_type;
static volatile bool   s_ble_up;          /* host synced: instance 0 is drivable */
static bool            s_adv_configured;  /* instance 0 params set once, then kept */
static xprsble_rx_cb_t s_rx_cb;

static uint32_t s_scan_results;
static int64_t  s_last_disc_us = -1;

/* ── Receive ─────────────────────────────────────────────────────────────── */

/*
 * The AD walk, shared by both backends because the wire is the same wire --
 * proven on air, a NimBLE deck and a tinynimble deck hearing each other in
 * both directions (common/tinynimble/README.md).
 *
 * Anything heavier than a memcpy belongs on another task. Under tinynimble
 * this runs in the CONTROLLER's context, which makes that rule stricter, not
 * looser.
 */
static void handle_ad(const uint8_t *p, int n, int rssi)
{
    /* Counted BEFORE any filtering: "the radio hears nothing" and "the radio
     * hears and none of it is ours" are different faults with different causes,
     * and they are indistinguishable without this. */
    s_scan_results++;
    s_last_disc_us = esp_timer_get_time();

    for (int i = 0; i + 2 <= n;) {
        int adlen = p[i];
        if (adlen == 0 || i + 1 + adlen > n) break;
        if (p[i + 1] == 0xFF && adlen >= 1 + 2) {
            const uint8_t *m = &p[i + 2];
            int mlen = adlen - 1;
            if (mlen >= 4 && m[0] == COMPANY_LO && m[1] == COMPANY_HI &&
                m[2] == MARKER && s_rx_cb) {
                s_rx_cb(m[3], &m[4], mlen - 4, rssi);
            }
        }
        i += 1 + adlen;
    }
}

#if CONFIG_XPRSBLE_BACKEND_TINYNIMBLE

static void tn_report(const tn_adv_report_t *r, void *ctx)
{
    (void)ctx;
    handle_ad(r->data, r->data_len, r->rssi);
}

#else

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_EXT_DISC) return 0;
    struct ble_gap_ext_disc_desc *d = &event->ext_disc;
    handle_ad(d->data, d->length_data, d->rssi);
    return 0;
}

#endif

/* ── Transmit ────────────────────────────────────────────────────────────── */

static int build_ad(uint8_t subtype, const uint8_t *payload, int len,
                    uint8_t *out)
{
    int n = 0;
    out[n++] = 0;            /* AD length placeholder */
    out[n++] = 0xFF;         /* manufacturer specific data */
    out[n++] = COMPANY_LO;
    out[n++] = COMPANY_HI;
    out[n++] = MARKER;
    out[n++] = subtype;
    if (n + len > 254) return 0;             /* one AD max 254 bytes */
    memcpy(out + n, payload, len); n += len;
    out[0] = (uint8_t)(n - 1);
    return n;
}

#if CONFIG_XPRSBLE_BACKEND_TINYNIMBLE

static void start_scan(void)
{
    tn_scan_cfg_t scan = {
        .own_addr_type = 0x01, .passive = 1,
        .itvl = 0x0060, .window = 0x0050, .phy = TN_PHY_1M,
    };
    esp_err_t err = tn_scan_start(&scan, tn_report, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
    else               ESP_LOGI(TAG, "extended scanning…");
}

/* tn_adv_set_data does stop -> set -> start itself, and does NOT recreate the
 * set. Recreating it is what makes the controller hand out a fresh random
 * address, and that churn fills peers' address books with several entries for
 * one station (docs/ble5.md §1). */
static bool air_raw_ad(const uint8_t *ad, int n)
{
    if (!s_ble_up) return false;
    esp_err_t err = tn_adv_set_data(ad, (size_t)n);
    if (err != ESP_OK) ESP_LOGE(TAG, "adv: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

#else   /* NimBLE */

static void start_scan(void)
{
    struct ble_gap_ext_disc_params uncoded = {
        .itvl = 0x0060, .window = 0x0050, .passive = 1,
    };
    int rc = ble_gap_ext_disc(s_own_addr_type, 0, 0, 0, 0, 0, &uncoded, NULL,
                              gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ext_disc rc=%d", rc);
    else         ESP_LOGI(TAG, "extended scanning…");
}

/*
 * Instance 0, configured on first use then kept. Stopping and recreating an
 * advertising set is what makes the controller hand out a fresh random address,
 * and that address churn is what fills peers' address books with several
 * entries for one station (docs/ble5.md §1).
 */
static bool air_raw_ad(const uint8_t *ad, int n)
{
    if (!s_ble_up) return false;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(ad, n);
    if (!om) { ESP_LOGW(TAG, "mbuf alloc failed"); return false; }
    if (!s_adv_configured) {
        struct ble_gap_ext_adv_params p = {0};
        p.connectable   = 0;
        p.scannable     = 0;
        p.legacy_pdu    = 0;
        p.own_addr_type = s_own_addr_type;
        p.primary_phy   = BLE_HCI_LE_PHY_1M;
        p.secondary_phy = BLE_HCI_LE_PHY_1M;
        p.sid           = 0;
        p.tx_power      = 127;
        p.itvl_min      = 0x100;   /* 160 ms */
        p.itvl_max      = 0x100;
        int rc = ble_gap_ext_adv_configure(0, &p, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ext_adv_configure rc=%d", rc);
            os_mbuf_free_chain(om);
            return false;
        }
        s_adv_configured = true;
    } else {
        ble_gap_ext_adv_stop(0);
    }
    int rc = ble_gap_ext_adv_set_data(0, om);
    if (rc != 0) { ESP_LOGE(TAG, "ext_adv_set_data rc=%d", rc); return false; }
    rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ext_adv_start rc=%d", rc);
        return false;
    }
    return true;
}

#endif  /* backend */

bool xprsble_send_sub(const uint8_t *payload, int len, uint8_t subtype)
{
    if (len <= 0 || len > XPRSBLE_WIRE_MAX) return false;
    uint8_t ad[256];
    int n = build_ad(subtype, payload, len, ad);
    if (n <= 0) return false;
    return air_raw_ad(ad, n);
}

bool xprsble_send(const char *wire, int len)
{
    if (!wire) return false;
    return xprsble_send_sub((const uint8_t *)wire, len, XPRSBLE_SUB_XPRS);
}

/* ── Bring-up ────────────────────────────────────────────────────────────── */

#if CONFIG_XPRSBLE_BACKEND_TINYNIMBLE

esp_err_t xprsble_start(const char *callsign)
{
    if (callsign) snprintf(s_call, sizeof s_call, "%s", callsign);

    esp_err_t err = tn_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tn_start: %s -- was WiFi started first?",
                 esp_err_to_name(err));
        return err;
    }

    /* One address, kept for the life of the station: see air_raw_ad(). */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    mac[0] |= 0xC0;                       /* static random */
    tn_set_random_addr(mac);
    s_own_addr_type = 0x01;

    tn_adv_cfg_t adv = {
        .handle = 0, .props = 0,          /* not connectable, not scannable */
        .itvl_min = 0x100, .itvl_max = 0x100,      /* 160 ms */
        .chan_map = 0x07, .own_addr_type = 0x01,
        .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M,
        .sid = 0,
    };
    err = tn_adv_configure(&adv);
    if (err != ESP_OK) { tn_stop(); return err; }
    s_adv_configured = true;

    /* No host task and no sync callback to wait for: the controller is up when
     * tn_start() returns, so there is no window in which the radio looks ready
     * and is not. */
    s_ble_up = true;
    start_scan();
    ESP_LOGI(TAG, "BLE5 up as %s (tinynimble)", s_call[0] ? s_call : "(no callsign)");
    return ESP_OK;
}

#else   /* NimBLE */

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
    s_ble_up = true;
    ESP_LOGI(TAG, "BLE5 up as %s", s_call[0] ? s_call : "(no callsign)");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset, reason=%d", reason);
    s_ble_up = false;
    s_adv_configured = false;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t xprsble_start(const char *callsign)
{
    if (callsign) snprintf(s_call, sizeof s_call, "%s", callsign);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    /* Registered BEFORE the host task starts. Doing it after is a race that
     * always loses: sync can fire before the callback is in place, and then
     * nothing ever brings the radio up while every counter looks healthy. */
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

#endif  /* backend */

bool xprsble_is_active(void) { return s_ble_up; }

void xprsble_set_rx_cb(xprsble_rx_cb_t cb) { s_rx_cb = cb; }

uint32_t xprsble_scan_results(void) { return s_scan_results; }

int xprsble_silent_for(void)
{
    if (s_last_disc_us < 0) return -1;
    return (int)((esp_timer_get_time() - s_last_disc_us) / 1000000);
}

#endif  /* CONFIG_BT_ENABLED */
