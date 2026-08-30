/*
 * tinynimble on the nRF52840 SoftDevice. See tinynimble.h.
 *
 * The other port (tn_port_esp.c) writes HCI packets to an ESP32 controller.
 * This one cannot: the S140 SoftDevice is not an HCI controller but a linked
 * binary reached through ARM SVC calls, with a GAP, a GATT client and a GATT
 * server of its own. So the SHAPE of tinynimble is what crosses over --
 * the same calls, the same semantics, a station's BLE code written once --
 * and the bytes underneath are the SoftDevice's, not tn_hci.c's.
 *
 * Why this exists rather than Bluefruit, which ships with the Arduino core:
 * Bluefruit sizes both its advert and its scan buffer to the legacy 31 bytes
 * at compile time, and an XPRS beacon is 112-173 (docs/ble5-nrf52.md). The
 * SoftDevice underneath does 255 and a 1650-byte scan buffer. This file goes
 * straight to it, which is exactly what tinynimble did to NimBLE.
 *
 * ROLES. The SoftDevice port DIALS (tn_gatt_dial): it is the central and the
 * GATT client, and because the mesh channel's handles are fixed (tn_att.h) it
 * needs no discovery -- it exchanges the MTU, subscribes at 0x0004 and writes
 * at 0x0006. Serving a peer that dials in would be the SoftDevice's own GATTS
 * with the same table, and is not written yet; tn_gatt_serve() says so.
 *
 * CONTEXT. SD_EVT_IRQHandler only raises a flag. Every event -- scan reports
 * included -- is read and delivered from tn_gatt_pump(), on the caller's own
 * task, which is the one place the ESP port and this one differ in what a
 * callback may do: here the report callback may take its time. It should
 * still not, so that code written for one port runs on the other.
 *
 * USB. The SoftDevice takes the POWER peripheral's interrupts with it, and
 * TinyUSB -- which is the serial console on this board -- has to be told
 * about VBUS from then on through SoC events. Forgetting this is a board
 * whose console dies the moment Bluetooth starts, which looks like a crash.
 */
#ifdef TN_PORT_SD

#include "tinynimble.h"
#include "tn_att.h"

#include <stdio.h>
#include <string.h>

#include "nrf.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "nrf_nvic.h"
#include "ble.h"
#include "ble_gap.h"
#include "ble_gattc.h"
#include "ble_gatts.h"
#include "ble_hci.h"
#include "nrfx_power.h"

extern void tusb_hal_nrf_power_event(uint32_t event);   /* TinyUSB, nRF port */

/* Errors are the SoftDevice's own NRF_ERROR_* codes, 0 = success. */
#define TN_ERR_STATE  NRF_ERROR_INVALID_STATE
#define TN_ERR_PARAM  NRF_ERROR_INVALID_PARAM

/* Connection configuration tag for our ATT MTU (BLE_CONN_CFG_TAG_DEFAULT is
 * the SoftDevice's 23-byte default and cannot be changed). */
#define CONN_CFG_TAG  1

static bool s_up;
static volatile bool s_evt_flag;

/* ── SoftDevice bring-up ─────────────────────────────────────────────── */

/*
 * THE ARDUINO CORE DOES NOT KNOW THE SOFTDEVICE IS RUNNING, and the place it
 * shows is the 1200-baud touch that PlatformIO uses to reflash: the core's
 * enterSerialDfu() (cores/nRF5/wiring.c) writes NRF_POWER->GPREGRET
 * directly, which is a register the SoftDevice restricts, so the SoftDevice
 * raises an application memory-access fault and the board hangs with a
 * dead USB port -- "Connection timed out" on the host, and a finger on the
 * RST button the only way back. Found the hard way on the P1-Pro.
 *
 * The write itself has already landed by the time this runs (the SoftDevice
 * watches the access, it does not block it), so the right thing to do with
 * that particular fault is exactly what the core was about to do next: reset.
 * The bootloader then reads the magic and waits for the upload. Every other
 * fault is a real one and stays on the console.
 */
static void sd_fault(uint32_t id, uint32_t pc, uint32_t info)
{
    if (id == NRF_FAULT_ID_APP_MEMACC) NVIC_SystemReset();
    printf("SoftDevice fault id=%lu pc=0x%08lx info=0x%08lx\n",
           (unsigned long)id, (unsigned long)pc, (unsigned long)info);
    for (;;) { }
}

void SD_EVT_IRQHandler(void) { s_evt_flag = true; }

tn_err_t tn_start(void)
{
    if (s_up) return TN_OK;

    /* The XIAO carries a 32 kHz crystal (variant.h: USE_LFXO). */
    nrf_clock_lf_cfg_t clock = {
        .source = NRF_CLOCK_LF_SRC_XTAL,
        .rc_ctiv = 0, .rc_temp_ctiv = 0,
        .accuracy = NRF_CLOCK_LF_ACCURACY_20_PPM,
    };
    uint8_t already = 0;
    sd_softdevice_is_enabled(&already);
    if (!already) {
        /* NRF_POWER is a peripheral the SoftDevice restricts, and TinyUSB has
         * its interrupt through nrfx_power for VBUS detection. Enable the
         * SoftDevice with that still held and it answers 4097,
         * NRF_ERROR_SDM_INCORRECT_INTERRUPT_CONFIGURATION, and says nothing
         * about which interrupt. Hand it back first; SoC events replace it
         * from here on (the USB block below). */
        nrfx_power_usbevt_disable();
        nrfx_power_usbevt_uninit();
        nrfx_power_uninit();
    }
    uint32_t err = already ? 0 : sd_softdevice_enable(&clock, sd_fault);
    if (err) { printf("tn: sd_softdevice_enable %lu\n", (unsigned long)err); return (int)err; }

    /* USB, as explained at the top. */
    sd_power_usbdetected_enable(true);
    sd_power_usbpwrrdy_enable(true);
    sd_power_usbremoved_enable(true);
    uint32_t usb_reg = 0;
    sd_power_usbregstatus_get(&usb_reg);
    if ((usb_reg & POWER_USBREGSTATUS_OUTPUTRDY_Msk) && NRF_USBD->USBPULLUP == 0)
        tusb_hal_nrf_power_event(NRFX_POWER_USB_EVT_READY);

    extern uint32_t __data_start__[];
    uint32_t ram_start = (uint32_t)__data_start__;
    ble_cfg_t cfg;

    /* One advertising set (the chip's maximum), one link in each role. */
    memset(&cfg, 0, sizeof cfg);
    cfg.gap_cfg.role_count_cfg.adv_set_count      = 1;
    cfg.gap_cfg.role_count_cfg.periph_role_count  = 1;
    cfg.gap_cfg.role_count_cfg.central_role_count = 1;
    cfg.gap_cfg.role_count_cfg.central_sec_count  = 0;
    err = sd_ble_cfg_set(BLE_GAP_CFG_ROLE_COUNT, &cfg, ram_start);
    if (err) { printf("tn: role count cfg %lu\n", (unsigned long)err); return (int)err; }

    /* The MTU tn_att.h is sized for, on our connections. */
    memset(&cfg, 0, sizeof cfg);
    cfg.conn_cfg.conn_cfg_tag = CONN_CFG_TAG;
    cfg.conn_cfg.params.gatt_conn_cfg.att_mtu = TN_ATT_MTU_MAX;
    err = sd_ble_cfg_set(BLE_CONN_CFG_GATT, &cfg, ram_start);
    if (err) { printf("tn: gatt cfg %lu\n", (unsigned long)err); return (int)err; }

    memset(&cfg, 0, sizeof cfg);
    cfg.conn_cfg.conn_cfg_tag = CONN_CFG_TAG;
    cfg.conn_cfg.params.gap_conn_cfg.conn_count   = 2;
    cfg.conn_cfg.params.gap_conn_cfg.event_length = 6;   /* 7.5 ms per event */
    err = sd_ble_cfg_set(BLE_CONN_CFG_GAP, &cfg, ram_start);
    if (err) { printf("tn: gap cfg %lu\n", (unsigned long)err); return (int)err; }

    /* Nothing is registered in the SoftDevice's own attribute table; make
     * its allocation as small as it will go. */
    memset(&cfg, 0, sizeof cfg);
    cfg.gatts_cfg.attr_tab_size.attr_tab_size = BLE_GATTS_ATTR_TAB_SIZE_MIN;
    sd_ble_cfg_set(BLE_GATTS_CFG_ATTR_TAB_SIZE, &cfg, ram_start);

    uint32_t need = ram_start;
    err = sd_ble_enable(&need);
    if (err) {
        /* The one failure worth spelling out: the linker gave the app RAM
         * from 0x20006000 and this configuration wants more below it. */
        printf("tn: sd_ble_enable %lu -- SoftDevice wants app RAM at 0x%08lx, "
               "linker gave 0x%08lx\n", (unsigned long)err,
               (unsigned long)need, (unsigned long)ram_start);
        return (int)err;
    }

    NVIC_SetPriority(SD_EVT_IRQn, 6);
    NVIC_EnableIRQ(SD_EVT_IRQn);
    s_up = true;
    printf("tn: SoftDevice up, app RAM from 0x%08lx (needs 0x%08lx)\n",
           (unsigned long)ram_start, (unsigned long)need);
    return TN_OK;
}

tn_err_t tn_stop(void)
{
    if (!s_up) return TN_OK;
    sd_softdevice_disable();
    s_up = false;
    return TN_OK;
}

bool tn_is_up(void) { return s_up; }

/* ── Address ─────────────────────────────────────────────────────────── */

tn_err_t tn_set_random_addr(const uint8_t addr[6])
{
    if (!s_up) return TN_ERR_STATE;
    ble_gap_addr_t a = { .addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC };
    memcpy(a.addr, addr, 6);
    return (int)sd_ble_gap_addr_set(&a);
}

/* ── Advertising: one set, stop -> configure -> start ────────────────── */

static tn_adv_cfg_t s_adv;
static bool         s_adv_configured, s_adv_on;
static uint8_t      s_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t      s_adv_buf[TN_ADV_DATA_MAX];

tn_err_t tn_adv_configure(const tn_adv_cfg_t *cfg)
{
    if (!s_up || !cfg) return TN_ERR_STATE;
    s_adv = *cfg;
    s_adv_configured = true;
    return TN_OK;      /* the SoftDevice takes params and data together, below */
}

static uint32_t adv_apply(const uint8_t *ad, size_t len)
{
    memcpy(s_adv_buf, ad, len);
    ble_gap_adv_data_t data = {
        .adv_data = { .p_data = s_adv_buf, .len = (uint16_t)len },
    };
    ble_gap_adv_params_t p;
    memset(&p, 0, sizeof p);
    p.properties.type = (s_adv.props & TN_ADV_PROP_CONNECTABLE)
        ? BLE_GAP_ADV_TYPE_EXTENDED_CONNECTABLE_NONSCANNABLE_UNDIRECTED
        : BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    p.interval      = s_adv.itvl_min ? s_adv.itvl_min : 0x100;
    p.duration      = BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED;
    p.filter_policy = BLE_GAP_ADV_FP_ANY;
    p.primary_phy   = BLE_GAP_PHY_1MBPS;
    p.secondary_phy = BLE_GAP_PHY_1MBPS;
    p.set_id        = s_adv.sid;
    return sd_ble_gap_adv_set_configure(&s_adv_handle, &data, &p);
}

tn_err_t tn_adv_set_data(const uint8_t *ad, size_t len)
{
    if (!s_up || !s_adv_configured) return TN_ERR_STATE;
    if (len > TN_ADV_DATA_MAX) return TN_ERR_PARAM;
    if (s_adv_on) { sd_ble_gap_adv_stop(s_adv_handle); s_adv_on = false; }
    uint32_t err = adv_apply(ad, len);
    if (err) { printf("tn: adv configure %lu\n", (unsigned long)err); return (int)err; }
    err = sd_ble_gap_adv_start(s_adv_handle, CONN_CFG_TAG);
    if (err) { printf("tn: adv start %lu\n", (unsigned long)err); return (int)err; }
    s_adv_on = true;
    return TN_OK;
}

tn_err_t tn_adv_stop(void)
{
    if (!s_up || !s_adv_on) return TN_OK;
    s_adv_on = false;
    return (int)sd_ble_gap_adv_stop(s_adv_handle);
}

/* ── Scanning ────────────────────────────────────────────────────────── */

static tn_report_cb_t s_rx_cb;
static void          *s_rx_ctx;
static bool           s_scan_want;         /* the caller wants it running */
static tn_scan_cfg_t  s_scan;
/* The minimum for an extended scan is 255 bytes; one advert's worth. */
static uint8_t        s_scan_buf[BLE_GAP_SCAN_BUFFER_EXTENDED_MIN];
static ble_data_t     s_scan_data = { s_scan_buf, sizeof s_scan_buf };

static void scan_params(ble_gap_scan_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->extended      = 1;
    p->active        = s_scan.passive ? 0 : 1;
    p->filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL;
    p->scan_phys     = BLE_GAP_PHY_1MBPS;
    p->interval      = s_scan.itvl   ? s_scan.itvl   : 0x0060;
    p->window        = s_scan.window ? s_scan.window : 0x0050;
    p->timeout       = BLE_GAP_SCAN_TIMEOUT_UNLIMITED;
}

static uint32_t scan_go(void)
{
    ble_gap_scan_params_t p;
    scan_params(&p);
    return sd_ble_gap_scan_start(&p, &s_scan_data);
}

tn_err_t tn_scan_start(const tn_scan_cfg_t *cfg, tn_report_cb_t cb, void *ctx)
{
    if (!s_up || !cfg) return TN_ERR_STATE;
    s_scan = *cfg;
    s_rx_cb = cb; s_rx_ctx = ctx;
    s_scan_want = true;
    uint32_t err = scan_go();
    if (err) printf("tn: scan start %lu\n", (unsigned long)err);
    return (int)err;
}

tn_err_t tn_scan_stop(void)
{
    if (!s_up) return TN_OK;
    s_scan_want = false;
    sd_ble_gap_scan_stop();
    return TN_OK;
}

/* ── The mesh channel, as the one who dials ─────────────────────────── */

static tn_gatt_cb_t s_gatt;
static volatile uint16_t s_conn = BLE_CONN_HANDLE_INVALID;
static uint16_t s_mtu = TN_ATT_MTU_DEFAULT;
static bool     s_dialling, s_subscribed;
static uint32_t s_dial_started_ms;

tn_err_t tn_gatt_serve(const tn_gatt_cb_t *cb)
{
    /* Serving on this port is the SoftDevice's GATTS with tn_att's table,
     * and it is not written. Say so rather than pretend. */
    (void)cb;
    return NRF_ERROR_NOT_SUPPORTED;
}

tn_err_t tn_gatt_dial(uint8_t addr_type, const uint8_t addr[6], const tn_gatt_cb_t *cb)
{
    if (!s_up || !addr || !cb) return TN_ERR_STATE;
    if (s_conn != BLE_CONN_HANDLE_INVALID || s_dialling) return TN_ERR_STATE;
    s_gatt = *cb;

    /* The initiator uses the scanner, so the scan has to stop first; it is
     * put back once the link is up or the attempt has failed. */
    sd_ble_gap_scan_stop();

    ble_gap_addr_t peer = { .addr_type = addr_type };
    memcpy(peer.addr, addr, 6);
    ble_gap_scan_params_t sp;
    scan_params(&sp);
    /* 30-50 ms interval, 4 s supervision: brisk, and gone quickly if the
     * far end walks off. */
    ble_gap_conn_params_t cp = {
        .min_conn_interval = 24, .max_conn_interval = 40,
        .slave_latency = 0, .conn_sup_timeout = 400,
    };
    uint32_t err = sd_ble_gap_connect(&peer, &sp, &cp, CONN_CFG_TAG);
    if (err) {
        printf("tn: connect %lu\n", (unsigned long)err);
        if (s_scan_want) scan_go();
        return (int)err;
    }
    s_dialling = true;
    s_subscribed = false;
    return TN_OK;
}

static void link_down(uint16_t conn, uint8_t reason)
{
    bool was = s_conn != BLE_CONN_HANDLE_INVALID;
    s_conn = BLE_CONN_HANDLE_INVALID;
    s_dialling = false;
    s_subscribed = false;
    s_mtu = TN_ATT_MTU_DEFAULT;
    if (was && s_gatt.disconnected) s_gatt.disconnected(s_gatt.ctx, conn, reason);
    if (s_scan_want) scan_go();
}

static void gattc_write(uint8_t op, uint16_t handle, const uint8_t *v, uint16_t len)
{
    ble_gattc_write_params_t w = {
        .write_op = op, .flags = 0, .handle = handle, .offset = 0,
        .len = len, .p_value = v,
    };
    uint32_t err = sd_ble_gattc_write(s_conn, &w);
    if (err) printf("tn: gattc write 0x%04x %lu\n", handle, (unsigned long)err);
}

static void on_ble_evt(const ble_evt_t *e)
{
    const ble_gap_evt_t   *gap = &e->evt.gap_evt;
    const ble_gattc_evt_t *gc  = &e->evt.gattc_evt;

    switch (e->header.evt_id) {
    case BLE_GAP_EVT_ADV_REPORT: {
        const ble_gap_evt_adv_report_t *r = &gap->params.adv_report;
        if (r->type.status != BLE_GAP_ADV_DATA_STATUS_INCOMPLETE_TRUNCATED && s_rx_cb) {
            tn_adv_report_t rep;
            memset(&rep, 0, sizeof rep);
            rep.evt_type  = (r->type.connectable ? 0x0001 : 0) |
                            (r->type.scannable ? 0x0002 : 0) |
                            (r->type.extended_pdu ? 0 : 0x0010);
            rep.addr_type = r->peer_addr.addr_type;
            memcpy(rep.addr, r->peer_addr.addr, 6);
            rep.rssi      = r->rssi;
            rep.tx_power  = r->tx_power;
            rep.sid       = r->set_id;
            rep.data      = r->data.p_data;
            rep.data_len  = (uint8_t)(r->data.len > 255 ? 255 : r->data.len);
            s_rx_cb(&rep, s_rx_ctx);
        }
        /* The SoftDevice hands back the buffer with each report and stops
         * until it is given one again. */
        if (s_scan_want && !s_dialling) sd_ble_gap_scan_start(NULL, &s_scan_data);
        break;
    }
    case BLE_GAP_EVT_CONNECTED:
        s_conn = gap->conn_handle;
        s_dialling = false;
        printf("tn: link 0x%04x up as %s\n", s_conn,
               gap->params.connected.role == BLE_GAP_ROLE_CENTRAL ? "central" : "peripheral");
        if (gap->params.connected.role == BLE_GAP_ROLE_CENTRAL) {
            /* Step 1 of 3: the MTU. Then the CCCD, then the caller hears. */
            uint32_t err = sd_ble_gattc_exchange_mtu_request(s_conn, TN_ATT_MTU_MAX);
            if (err) printf("tn: mtu request %lu\n", (unsigned long)err);
        }
        if (s_scan_want) scan_go();      /* the scanner is free again */
        break;
    case BLE_GAP_EVT_DISCONNECTED:
        printf("tn: link 0x%04x down (0x%02x)\n", gap->conn_handle,
               gap->params.disconnected.reason);
        link_down(gap->conn_handle, gap->params.disconnected.reason);
        break;
    case BLE_GAP_EVT_TIMEOUT:
        if (gap->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN) {
            printf("tn: dial timed out\n");
            link_down(BLE_CONN_HANDLE_INVALID, 0);
        } else if (gap->params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN && s_scan_want) {
            scan_go();
        }
        break;

    /* The link-layer negotiations a central is asked about. Say yes. */
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:
        sd_ble_gap_data_length_update(gap->conn_handle, NULL, NULL);
        break;
    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
        ble_gap_phys_t phys = { BLE_GAP_PHY_AUTO, BLE_GAP_PHY_AUTO };
        sd_ble_gap_phy_update(gap->conn_handle, &phys);
        break;
    }
    case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
        sd_ble_gap_conn_param_update(gap->conn_handle,
            &gap->params.conn_param_update_request.conn_params);
        break;
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        /* No pairing on this channel: the packets carry their own signatures
         * (docs/ble5-gatt.md, "a connection is private, not authentic"). */
        sd_ble_gap_sec_params_reply(gap->conn_handle,
            BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
        break;
    case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
        sd_ble_gatts_exchange_mtu_reply(e->evt.gatts_evt.conn_handle, TN_ATT_MTU_MAX);
        break;

    case BLE_GATTC_EVT_EXCHANGE_MTU_RSP: {
        uint16_t theirs = gc->params.exchange_mtu_rsp.server_rx_mtu;
        s_mtu = theirs < TN_ATT_MTU_MAX ? theirs : TN_ATT_MTU_MAX;
        if (s_mtu < TN_ATT_MTU_DEFAULT) s_mtu = TN_ATT_MTU_DEFAULT;
        printf("tn: mtu %u\n", s_mtu);
        /* Step 2: subscribe. Fixed handle; no discovery. */
        static const uint8_t on[2] = { 0x01, 0x00 };
        gattc_write(BLE_GATT_OP_WRITE_REQ, TN_H_NOTIFY_CCCD, on, 2);
        break;
    }
    case BLE_GATTC_EVT_WRITE_RSP:
        if (gc->gatt_status != BLE_GATT_STATUS_SUCCESS) {
            printf("tn: write to 0x%04x refused, gatt status 0x%04x\n",
                   gc->error_handle, gc->gatt_status);
            break;
        }
        if (!s_subscribed) {
            /* Step 3: the caller may talk now. */
            s_subscribed = true;
            if (s_gatt.connected) s_gatt.connected(s_gatt.ctx, s_conn, true);
        }
        break;
    case BLE_GATTC_EVT_HVX:
        if (gc->params.hvx.handle == TN_H_NOTIFY_VAL && s_gatt.rx)
            s_gatt.rx(s_gatt.ctx, gc->params.hvx.data, gc->params.hvx.len);
        break;
    case BLE_GATTC_EVT_TIMEOUT:
        printf("tn: gattc timeout, dropping the link\n");
        sd_ble_gap_disconnect(gc->conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        break;
    default:
        break;
    }
}

void tn_gatt_pump(void)
{
    if (!s_up) return;
    s_evt_flag = false;

    /* Every BLE event, then every SoC event. Sized for the largest event
     * our MTU can produce, 4-byte aligned as the SoftDevice requires. */
    static uint32_t evt[(BLE_EVT_LEN_MAX(TN_ATT_MTU_MAX) + 3) / 4];
    for (;;) {
        uint16_t len = sizeof evt;
        uint32_t err = sd_ble_evt_get((uint8_t *)evt, &len);
        if (err == NRF_ERROR_NOT_FOUND) break;
        if (err) { printf("tn: evt_get %lu\n", (unsigned long)err); break; }
        on_ble_evt((const ble_evt_t *)evt);
    }
    uint32_t soc;
    while (sd_evt_get(&soc) == NRF_SUCCESS) {
        int32_t usb = soc == NRF_EVT_POWER_USB_DETECTED    ? NRFX_POWER_USB_EVT_DETECTED :
                      soc == NRF_EVT_POWER_USB_POWER_READY ? NRFX_POWER_USB_EVT_READY :
                      soc == NRF_EVT_POWER_USB_REMOVED     ? NRFX_POWER_USB_EVT_REMOVED : -1;
        if (usb >= 0) tusb_hal_nrf_power_event((uint32_t)usb);
    }
}

bool tn_gatt_connected(void) { return s_conn != BLE_CONN_HANDLE_INVALID && s_subscribed; }
int  tn_gatt_mtu(void)       { return s_mtu - 3; }

tn_err_t tn_gatt_send(const uint8_t *data, int len)
{
    if (!tn_gatt_connected()) return TN_ERR_STATE;
    if (len < 1 || len > s_mtu - 3) return TN_ERR_PARAM;
    ble_gattc_write_params_t w = {
        .write_op = BLE_GATT_OP_WRITE_CMD, .flags = 0,
        .handle = TN_H_WRITE_VAL, .offset = 0,
        .len = (uint16_t)len, .p_value = data,
    };
    /* NRF_ERROR_RESOURCES here means the one-deep write-command queue is
     * still draining; the caller waits and tries again, which is what
     * BLEMESH_SEND_BUSY is for. */
    return (int)sd_ble_gattc_write(s_conn, &w);
}

tn_err_t tn_gatt_disconnect(void)
{
    if (s_conn == BLE_CONN_HANDLE_INVALID) return TN_OK;
    return (int)sd_ble_gap_disconnect(s_conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
}

#endif /* TN_PORT_SD */
