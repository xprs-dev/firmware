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
#include "tn_att.h"

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
static volatile uint16_t s_conn = 0xFFFF;   /* the mesh channel's link, below */

/* ── VHCI callbacks, both in controller context ─────────────────────────── */

static void vhci_send_available(void)
{
    if (s_can_send) xSemaphoreGive(s_can_send);
}

/* ── ACL credits ─────────────────────────────────────────────────────────
 *
 * esp_vhci_host_check_send_available() answers for the VHCI PIPE, not for the
 * controller's ACL buffer pool. HCI's contract is that the host counts
 * Number Of Completed Packets (event 0x13) credits and never holds more
 * packets outstanding than the controller has buffers -- send past that and
 * the controller DISCARDS, silently. The old one-PDU-in-flight users never
 * felt it; a bulk blast (xprs_blob) lost 99% of its frames to exactly this.
 *
 * The pool is worth 8 on this controller config; 6 keeps clear of it while
 * still filling a connection event. */
#define TN_ACL_CREDITS 6
static volatile int s_acl_credits = TN_ACL_CREDITS;

static volatile uint32_t s_nocp_events, s_nocp_packets;   /* diagnostics */
static void acl_on_completed(const uint8_t *pkt, uint16_t len)
{
    /* 04 13 plen num { handle u16, completed u16 }* */
    if (len < 4 || pkt[0] != 0x04 || pkt[1] != 0x13) return;
    int num = pkt[3];
    const uint8_t *p = pkt + 4;
    int total = 0;
    for (int i = 0; i < num && (p + 4) <= pkt + len; i++, p += 4)
        total += p[2] | (p[3] << 8);
    if (total > 0) {
        s_nocp_events++; s_nocp_packets += (uint32_t)total;
        int c = s_acl_credits + total;
        s_acl_credits = c > TN_ACL_CREDITS ? TN_ACL_CREDITS : c;
    }
}


static void gatt_on_acl(const uint8_t *pkt, uint16_t len);      /* below */
static void gatt_on_link(const tn_link_evt_t *e, void *ctx);

static int vhci_recv(uint8_t *data, uint16_t len)
{
    /* A command result unblocks tn_send_cmd(); an advertising report goes
     * straight to the caller; ACL data and link events are PARKED for
     * tn_gatt_pump(), because answering an ATT request means sending, and
     * sending from the controller's own callback is not a thing this port
     * will do. Everything else the controller says is ignored. */
    if (len > 0 && data[0] == TN_H4_ACL) { gatt_on_acl(data, len); return 0; }
    acl_on_completed(data, len);            /* ACL credits back from the controller */
    uint8_t status;
    if (tn_hci_cmd_result(data, len, s_cmd_opcode, &status)) {
        s_cmd_status = status;
        if (s_cmd_done) xSemaphoreGive(s_cmd_done);
        return 0;
    }
    if (tn_hci_feed_link(data, len, gatt_on_link, NULL) != 0) return 0;
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
    s_conn = 0xFFFF;

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

    /* Big LL packets by default, so a peer's data-length request gets 251 and
     * not this controller's 27-byte default (a 244-byte notification then
     * rides one packet instead of ten). Best-effort: a controller that
     * refuses simply stays fragmented. */
    n = tn_hci_le_write_default_data_len(buf, sizeof buf, 251, 2120);
    if (n > 0) tn_send_cmd(buf, n, TN_OP_LE_WRITE_DEFAULT_DATA_LEN);

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
    int n;
    /* Parameters cannot change on an ENABLED set: the controller answers
     * 0x0C Command Disallowed, which is what turning the beacon connectable
     * ran into. Disable first; the next tn_adv_set_data() re-enables. */
    if (s_adv_configured) {
        n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, false);
        if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);
    }
    n = tn_hci_ext_adv_params(buf, sizeof buf, cfg);
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
     * every peer's address book (see common/xprs_bearer_ble/xprsble.c). */
    static uint8_t buf[4 + 4 + TN_ADV_DATA_MAX];
    int n;

    n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, false);
    if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);

    n = tn_hci_ext_adv_data(buf, sizeof buf, s_adv.handle, ad, len);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = tn_send_cmd(buf, n, TN_OP_EXT_ADV_DATA);
    if (err != ESP_OK) return err;

    /* A CONNECTABLE set cannot be enabled while its link is up: the
     * controller has one activity slot for "this set's connection" and it
     * is in use, so the enable answers 0x07 Memory Capacity Exceeded, every
     * beacon, for the whole session. The data is set and stays set; the
     * pump enables the set again the moment the link drops. While connected
     * this station is therefore silent on the broadcast plane -- which
     * docs/ble5-gatt.md already asks sessions to be short for. */
    if (s_conn != 0xFFFF && (s_adv.props & TN_ADV_PROP_CONNECTABLE)) return ESP_OK;

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

/* ── The mesh channel over a connection ─────────────────────────────────
 *
 * The controller opens the link when a peer answers the connectable set;
 * from then on ACL packets arrive in vhci_recv and are parked here, one
 * L2CAP frame at a time, for tn_gatt_pump() to answer from the caller's
 * task. ATT is strictly request/response, so one slot is enough for every
 * request; a Write Command that lands while the slot is full is dropped and
 * counted, which MSP tolerates (it re-sends what was not acked).
 *
 * One connection, because the T-Deck's controller is configured for one
 * (CONFIG_BT_CTRL_BLE_MAX_ACT=2: the advertising set and one link) and one
 * is the whole design -- docs/ble5-gatt.md, "a connected station is partly
 * deaf". */
static tn_gatt_cb_t     s_gatt;
static bool             s_gatt_serving;
static tn_att_t         s_att;

/* Parked inbound L2CAP frame, assembled across HCI fragments. */
/* A RING of parked frames, not a single slot. A central may put several
 * write commands in one connection event (a reply and a START back-to-back
 * is the xprs_blob handshake); with one slot the second was dropped before
 * the pump ever ran, and the session died on a frame the link had in fact
 * delivered. Single producer (controller context) / single consumer (pump). */
#define TN_IN_SLOTS 4
static struct {
    uint8_t       buf[4 + TN_ATT_MTU_MAX];
    int           len;        /* bytes so far */
    int           want;       /* 4 + L2CAP length, once known */
    volatile bool ready;
} s_ins[TN_IN_SLOTS];
static volatile int      s_in_w, s_in_r;
static volatile uint32_t s_in_dropped;

/* Parked link events, delivered in order from the pump. */
static volatile uint8_t s_ev_conn, s_ev_disc, s_ev_reason;
static volatile uint16_t s_ev_handle;

static void gatt_on_link(const tn_link_evt_t *e, void *ctx)
{
    (void)ctx;
    if (e->connected) {
        s_conn = e->conn;
        s_ev_handle = e->conn;
        s_ev_conn = 1;
        s_acl_credits = TN_ACL_CREDITS;
        for (int i = 0; i < TN_IN_SLOTS; i++) { s_ins[i].len = 0; s_ins[i].want = 0; s_ins[i].ready = false; }
        s_in_w = s_in_r = 0;
    } else if (e->conn == s_conn) {
        s_conn = 0xFFFF;
        s_ev_handle = e->conn;
        s_ev_reason = e->reason;
        s_ev_disc = 1;
    }
}

static void gatt_on_acl(const uint8_t *pkt, uint16_t len)
{
    uint16_t conn; uint8_t pb; const uint8_t *d;
    int n = tn_hci_acl_decode(pkt, len, &conn, &pb, &d);
    if (n < 0 || conn != s_conn) return;
    if (pb != TN_ACL_PB_CONT) {
        if (s_ins[s_in_w].ready) { s_in_dropped++; return; }   /* ring full */
        if (n < 4) return;
        s_ins[s_in_w].len = 0;
        s_ins[s_in_w].want = 4 + (int)(d[0] | (d[1] << 8));
        if (s_ins[s_in_w].want > (int)sizeof s_ins[s_in_w].buf) { s_ins[s_in_w].want = 0; return; }
    } else if (s_ins[s_in_w].want == 0) {
        return;                                          /* continuation of nothing */
    }
    if (s_ins[s_in_w].len + n > s_ins[s_in_w].want) return;
    memcpy(s_ins[s_in_w].buf + s_ins[s_in_w].len, d, n);
    s_ins[s_in_w].len += n;
    if (s_ins[s_in_w].len == s_ins[s_in_w].want) {
        s_ins[s_in_w].ready = true;                     /* publish, then advance */
        s_in_w = (s_in_w + 1) % TN_IN_SLOTS;
    }
}

static esp_err_t gatt_send_l2cap(const uint8_t *pdu, int len)
{
    static uint8_t acl[5 + 4 + TN_ATT_MTU_MAX];
    uint8_t l2[4 + TN_ATT_MTU_MAX];
    int m = tn_l2cap_wrap(l2, sizeof l2, TN_L2CAP_CID_ATT, pdu, len);
    if (m < 0) return ESP_ERR_INVALID_SIZE;
    int n = tn_hci_acl_encode(acl, sizeof acl, s_conn, TN_ACL_PB_FIRST_NONFLUSH, l2, m);
    if (n < 0) return ESP_ERR_INVALID_SIZE;
    /* Two gates. The VHCI pipe must be free, AND a controller ACL buffer
     * must be spoken for (see TN_ACL_CREDITS above) -- without the second a
     * bulk sender overruns the pool and the controller discards in silence.
     * No credit is not an error, it is "later": the caller pauses and
     * resumes when tn_gatt_pump has drained the completions. */
    if (s_acl_credits <= 0) return ESP_ERR_NOT_FINISHED;   /* busy: retry later */
    if (!esp_vhci_host_check_send_available() &&
        xSemaphoreTake(s_can_send, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    s_acl_credits--;
    esp_vhci_host_send_packet(acl, (uint16_t)n);
    return ESP_OK;
}

static void gatt_on_write(void *ctx, const uint8_t *data, int len)
{
    (void)ctx;
    if (s_gatt.rx) s_gatt.rx(s_gatt.ctx, data, len);
}

tn_err_t tn_gatt_serve(const tn_gatt_cb_t *cb)
{
    if (!s_up || !cb) return ESP_ERR_INVALID_STATE;
    s_gatt = *cb;
    s_gatt_serving = true;
    tn_att_init(&s_att);
    return ESP_OK;
}

tn_err_t tn_gatt_dial(uint8_t addr_type, const uint8_t addr[6], const tn_gatt_cb_t *cb)
{
    /* Central role is not written for this port -- the ESP32 stations are
     * dialled by phones and by the SoftDevice port, not the other way. */
    (void)addr_type; (void)addr; (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

void tn_gatt_pump(void)
{
    if (!s_gatt_serving) return;

    if (s_ev_conn) {
        s_ev_conn = 0;
        tn_att_init(&s_att);
        ESP_LOGI(TAG, "link 0x%04x up", s_ev_handle);
        if (s_gatt.connected) s_gatt.connected(s_gatt.ctx, s_ev_handle, false);
    }
    while (s_ins[s_in_r].ready) {
        int slot = s_in_r;
        uint16_t cid; const uint8_t *pdu;
        int n = tn_l2cap_unwrap(s_ins[slot].buf, s_ins[slot].len, &cid, &pdu);
        uint8_t out[TN_ATT_MTU_MAX];
        int m = 0;
        if (n > 0 && cid == TN_L2CAP_CID_ATT)
            m = tn_att_handle(&s_att, pdu, n, out, sizeof out, gatt_on_write, NULL);
        s_ins[slot].len = 0; s_ins[slot].want = 0;
        s_ins[slot].ready = false;           /* slot free before the send blocks */
        s_in_r = (slot + 1) % TN_IN_SLOTS;
        if (m > 0) gatt_send_l2cap(out, m);
    }
    if (s_ev_disc) {
        s_ev_disc = 0;
        ESP_LOGI(TAG, "link 0x%04x down (0x%02x)", s_ev_handle, s_ev_reason);
        if (s_gatt.disconnected) s_gatt.disconnected(s_gatt.ctx, s_ev_handle, s_ev_reason);
        /* A connectable set stops advertising the moment it is answered
         * (Vol 6 Part B, 4.4.2.4). Put it back, so the next peer can find
         * us; the beacon rotation would do this within seconds anyway, but a
         * station that is quiet for a while should not be invisible. */
        if (s_adv_configured) {
            uint8_t buf[16];
            int n = tn_hci_ext_adv_enable(buf, sizeof buf, s_adv.handle, true);
            if (n > 0) tn_send_cmd(buf, n, TN_OP_EXT_ADV_ENABLE);
        }
    }
}

bool tn_gatt_connected(void) { return s_conn != 0xFFFF; }
int  tn_gatt_mtu(void)       { return tn_att_notify_max(&s_att); }

tn_err_t tn_gatt_send(const uint8_t *data, int len)
{
    if (!tn_gatt_connected()) return ESP_ERR_INVALID_STATE;
    uint8_t pdu[TN_ATT_MTU_MAX];
    int n = tn_att_notify(&s_att, data, len, pdu, sizeof pdu);
    if (n < 0) return ESP_ERR_INVALID_SIZE;    /* not subscribed, or too long */
    return gatt_send_l2cap(pdu, n);
}

tn_err_t tn_gatt_disconnect(void)
{
    if (!tn_gatt_connected()) return ESP_OK;
    uint8_t buf[16];
    int n = tn_hci_disconnect(buf, sizeof buf, s_conn, TN_HCI_ERR_REMOTE_USER_TERM);
    if (n < 0) return ESP_ERR_INVALID_ARG;
    return tn_send_cmd(buf, n, TN_OP_DISCONNECT);
}
/* Diagnostics for the probe's status line. */
void tn_acl_stats(uint32_t *nocp_events, uint32_t *nocp_packets, int *credits, uint32_t *in_dropped)
{
    if (nocp_events)  *nocp_events  = s_nocp_events;
    if (nocp_packets) *nocp_packets = s_nocp_packets;
    if (credits)      *credits      = s_acl_credits;
    if (in_dropped)   *in_dropped   = s_in_dropped;
}
