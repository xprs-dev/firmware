/**
 * @file tinynimble.h
 * @brief The BLE5 surface XPRS actually uses, spoken directly to the controller.
 *
 * NimBLE gives this firmware six things it needs and a great deal it does not.
 * Measured on the T-Dongle-S3, the host's security manager, ATT/GATT, L2CAP and
 * bonding store are 37,760 bytes of text with **zero callers anywhere in the
 * tree** -- the security manager is only enabled at all to work around an IDF
 * 5.2.1 compile bug (see models/tdeck/firmware/sdkconfig.defaults). What the
 * stations do is broadcast and listen:
 *
 *   - one extended advertising set, non-connectable, non-scannable, 1M PHY
 *   - one extended passive scan, uncoded, no duplicate filtering
 *   - and the ability to hand the radio back entirely, because with the BLE
 *     controller up an unassociated WiFi station receives nothing (docs/espnow.md)
 *
 * That is the whole of it, and it is six HCI commands and one event.
 *
 * WHAT THIS DOES NOT SAVE, SO NOBODY EXPECTS IT TO
 * -----------------------------------------------
 * The link-layer CONTROLLER stays. It is a binary blob with microsecond
 * deadlines and its pools come out of internal DRAM whatever anyone does --
 * the T-Deck config records this after finding out the hard way. tinynimble
 * removes the HOST's share: the msys mbuf pools, the ACL transport buffers and
 * the 5,120-byte host task stack. On a board with PSRAM,
 * CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL already moves those outward for one
 * Kconfig line, so this is worth building for the boards WITHOUT PSRAM: the
 * T-Dongle-S3 and the C3-mini, which are also the two tightest in the fleet.
 *
 * LAYERING
 * --------
 * tn_hci.c is pure encode/decode over caller-owned buffers -- no ESP-IDF, no
 * heap, no tasks -- so the bytes that go on the wire are checked on a desk
 * against captures from the working NimBLE build. Same arrangement as
 * xprs_codec and xs_bundle, and for the same reason: the one thing that must
 * not drift is what the controller is told.
 *
 * tn_port_esp.c is the only file that knows about esp_bt_controller and
 * esp_vhci.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An extended advertising PDU can carry 254 bytes in one AD structure. The
 * chain cap is 1650, but every XPRS frame is kept to a single AD so that phones
 * with ~247-byte controller caps hear it (docs/esp32.md). */
#define TN_ADV_DATA_MAX     254

/* H4 packet indicators, the first byte on the VHCI wire. */
#define TN_H4_CMD           0x01
#define TN_H4_ACL           0x02
#define TN_H4_EVT           0x04

/* The six opcodes. OGF 0x08 (LE Controller) except where noted. */
#define TN_OP_RESET             0x0C03
#define TN_OP_SET_EVENT_MASK    0x0C01
#define TN_OP_LE_SET_EVENT_MASK 0x2001
#define TN_OP_SET_RANDOM_ADDR   0x2005
#define TN_OP_ADV_SET_RAND_ADDR 0x2035
#define TN_OP_EXT_ADV_PARAMS    0x2036
#define TN_OP_EXT_ADV_DATA      0x2037
#define TN_OP_EXT_ADV_ENABLE    0x2039
#define TN_OP_EXT_SCAN_PARAMS   0x2041
#define TN_OP_EXT_SCAN_ENABLE   0x2042
/* Connections (docs/ble5-gatt.md). The controller opens them by itself when a
 * peer answers a connectable set; what the host owes it is a way to close
 * one, and the ACL packet type that carries the ATT PDUs across it. */
#define TN_OP_DISCONNECT        0x0406

/* ACL Packet_Boundary flags (Vol 4 Part E, 5.4.2). Host -> controller sends
 * FIRST_NONFLUSH; the controller sends FIRST_FLUSH for the start of an L2CAP
 * PDU and CONT for the rest when its buffer is smaller than the PDU. */
#define TN_ACL_PB_FIRST_NONFLUSH 0x00
#define TN_ACL_PB_CONT           0x01
#define TN_ACL_PB_FIRST_FLUSH    0x02
#define TN_HCI_ERR_REMOTE_USER_TERM 0x13

/* PHY selectors. Every parameter block in this firmware hardcodes 1M; 2M and
 * coded are enabled in Kconfig but never selected. */
#define TN_PHY_1M           0x01
#define TN_PHY_2M           0x02
#define TN_PHY_CODED        0x03

/* Advertising_Event_Properties bits. XPRS broadcasts use none of them: not
 * connectable, not scannable, not directed, not legacy, not anonymous. */
#define TN_ADV_PROP_CONNECTABLE  0x0001
#define TN_ADV_PROP_SCANNABLE    0x0002
#define TN_ADV_PROP_DIRECTED     0x0004
#define TN_ADV_PROP_LEGACY       0x0010
#define TN_ADV_PROP_ANONYMOUS    0x0020
#define TN_ADV_PROP_TX_POWER     0x0040

typedef struct {
    uint8_t  handle;         /* advertising set, 0 for the XPRS broadcast set */
    uint16_t props;          /* TN_ADV_PROP_*; 0 for a plain broadcast        */
    uint32_t itvl_min;       /* 0.625 ms units. 0x100 = 160 ms in this tree   */
    uint32_t itvl_max;
    uint8_t  chan_map;       /* 0x07 = all three primary channels             */
    uint8_t  own_addr_type;  /* whatever the controller inferred              */
    int8_t   tx_power;       /* 127 = "no preference", i.e. controller max    */
    uint8_t  primary_phy;
    uint8_t  secondary_phy;
    uint8_t  sid;
} tn_adv_cfg_t;

typedef struct {
    uint8_t  own_addr_type;
    uint8_t  passive;        /* 1 = passive; XPRS never scan-requests         */
    uint16_t itvl;           /* 0.625 ms units. 0x0060 / 0x0050 = ~83% duty   */
    uint16_t window;
    uint8_t  phy;            /* TN_PHY_1M                                     */
} tn_scan_cfg_t;

/* One extended advertising report, pointing into the caller's packet buffer.
 * Zero-copy on purpose: nothing here allocates, and the data is valid only for
 * the duration of the callback. */
typedef struct {
    uint16_t       evt_type;
    uint8_t        addr_type;
    uint8_t        addr[6];
    int8_t         rssi;
    int8_t         tx_power;
    uint8_t        sid;
    const uint8_t *data;
    uint8_t        data_len;
} tn_adv_report_t;

typedef void (*tn_report_cb_t)(const tn_adv_report_t *r, void *ctx);

/* ── Command encoders ───────────────────────────────────────────────────────
 *
 * Each writes a complete H4 command packet (indicator, opcode, length,
 * parameters) into `buf` and returns its length, or -1 if it does not fit.
 * Nothing allocates; the caller owns the buffer. */

/* The bring-up nobody mentions until nothing arrives.
 *
 * Both event masks default to EXCLUDING what a scanner needs. Set_Event_Mask
 * defaults to bits 0..44, so LE Meta (bit 61) -- the envelope every LE event
 * travels in -- is off. LE_Set_Event_Mask defaults to bits 0..4, so Extended
 * Advertising Report (bit 12) is off. The controller will happily accept
 * scan parameters and a scan enable with both masked, report no error, and
 * deliver nothing at all. */
int tn_hci_reset(uint8_t *buf, size_t cap);
int tn_hci_set_event_mask(uint8_t *buf, size_t cap, uint64_t mask);
int tn_hci_le_set_event_mask(uint8_t *buf, size_t cap, uint64_t mask);

/* Defaults, plus the bit each mask is missing. */
#define TN_EVENT_MASK_DEFAULT     0x00001FFFFFFFFFFFULL
#define TN_EVENT_MASK_LE_META     (1ULL << 61)
#define TN_LE_EVENT_MASK_DEFAULT  0x000000000000001FULL
#define TN_LE_EVENT_MASK_EXT_ADV  (1ULL << 12)

int tn_hci_set_random_addr(uint8_t *buf, size_t cap, const uint8_t addr[6]);

/* LE Set Advertising Set Random Address (0x2035).
 *
 * NOT the same command as 0x2005, and the difference costs a boot to find:
 * an EXTENDED advertising set carries its own address, and 0x2005 does not
 * reach it. Configure a set with own_addr_type = random and no per-set address
 * and the controller accepts the parameters, accepts the data, and then refuses
 * LE Set Extended Advertising Enable with 0x12 "Invalid HCI Command
 * Parameters" -- pointing at the enable, not at the missing address. */
int tn_hci_adv_set_random_addr(uint8_t *buf, size_t cap, uint8_t handle,
                               const uint8_t addr[6]);
int tn_hci_ext_adv_params (uint8_t *buf, size_t cap, const tn_adv_cfg_t *cfg);
int tn_hci_ext_adv_data   (uint8_t *buf, size_t cap, uint8_t handle,
                           const uint8_t *ad, size_t ad_len);
int tn_hci_ext_adv_enable (uint8_t *buf, size_t cap, uint8_t handle, bool on);
int tn_hci_ext_scan_params(uint8_t *buf, size_t cap, const tn_scan_cfg_t *cfg);
int tn_hci_ext_scan_enable(uint8_t *buf, size_t cap, bool on, bool filter_dup);

/* ── Event decoding ─────────────────────────────────────────────────────────
 *
 * Feed whole H4 event packets. Extended advertising reports are handed to `cb`
 * one at a time; everything else is ignored, which is the entire event policy.
 *
 * Returns the number of reports delivered, or -1 if the packet is malformed --
 * and it is checked rather than trusted, because a truncated report from a
 * controller must not become an out-of-range read. */
int tn_hci_feed_evt(const uint8_t *pkt, size_t len, tn_report_cb_t cb, void *ctx);

/* True if this event is a Command Complete/Status for `opcode`; `status` is
 * filled with the controller's verdict. How bring-up knows a step took. */
bool tn_hci_cmd_result(const uint8_t *pkt, size_t len,
                       uint16_t opcode, uint8_t *status);

/* ── Connections ────────────────────────────────────────────────────────────
 *
 * Two events tell the whole story of a link: it opened, it closed. Both
 * flavours of the first are decoded -- LE Connection Complete (subevent
 * 0x01, what the default LE event mask delivers) and LE Enhanced Connection
 * Complete (0x0A, if a port ever enables bit 9) -- so the port does not have
 * to care which the controller chose. */
typedef struct {
    bool     connected;     /* false: disconnected, and `reason` says why */
    uint16_t conn;          /* 12-bit connection handle */
    uint8_t  role;          /* 0 central, 1 peripheral */
    uint8_t  peer_addr_type;
    uint8_t  peer_addr[6];
    uint8_t  reason;        /* HCI error code, disconnection only */
} tn_link_evt_t;
typedef void (*tn_link_cb_t)(const tn_link_evt_t *e, void *ctx);

/* Returns 1 when the packet was a link event and `cb` was called, 0 when it
 * was something else, -1 when it claimed to be one and was too short. */
int tn_hci_feed_link(const uint8_t *pkt, size_t len, tn_link_cb_t cb, void *ctx);

int tn_hci_disconnect(uint8_t *buf, size_t cap, uint16_t conn, uint8_t reason);

/* H4 ACL data packet: indicator, handle | PB<<12 | BC<<14, length, data.
 * One L2CAP frame per call; ATT_MTU is kept small enough that one is
 * enough (tn_att.h). */
int tn_hci_acl_encode(uint8_t *buf, size_t cap, uint16_t conn, uint8_t pb,
                      const uint8_t *data, int len);
/* Returns the data length and points `data` into `pkt`, or -1 when the
 * packet's own length field claims more than it carries. */
int tn_hci_acl_decode(const uint8_t *pkt, size_t len, uint16_t *conn,
                      uint8_t *pb, const uint8_t **data);

/* ── The radio, on ESP-IDF (tn_port_esp.c) ──────────────────────────────────
 *
 * Not built on the host; the encoders above are the host-testable half.
 *
 * NOTE ON CONTEXT: the report callback passed to tn_scan_start() runs in the
 * CONTROLLER's context. Parse, identify, deduplicate and park -- nothing that
 * blocks, allocates without bound, or takes a lock another task holds. This is
 * the rule docs/esp32.md states for every receive path here, and it is stricter
 * in this one because the context belongs to the link layer. */
/* Two ports implement everything below: tn_port_esp.c over the ESP32
 * controller's VHCI, and tn_port_sd.c over the nRF52840 SoftDevice
 * (models/sensecap-p1-pro). Same calls, same semantics, so a station's BLE
 * code is written once. The error type is the port's own; 0 is success on
 * both and the only value a caller compares against. */
#ifdef ESP_PLATFORM
#include "esp_err.h"
typedef esp_err_t tn_err_t;
#else
typedef int tn_err_t;
#endif
#define TN_OK 0

#if defined(ESP_PLATFORM) || defined(TN_PORT_SD)

tn_err_t tn_start(void);           /* controller up. Call BEFORE WiFi.      */
tn_err_t tn_stop(void);            /* full teardown; gives the radio back   */
bool      tn_is_up(void);

tn_err_t tn_set_random_addr(const uint8_t addr[6]);

tn_err_t tn_adv_configure(const tn_adv_cfg_t *cfg);   /* once               */
tn_err_t tn_adv_set_data(const uint8_t *ad, size_t len); /* stop→set→start  */
tn_err_t tn_adv_stop(void);

tn_err_t tn_scan_start(const tn_scan_cfg_t *cfg, tn_report_cb_t cb, void *ctx);
tn_err_t tn_scan_stop(void);

/* ── The mesh channel over a connection (docs/ble5-gatt.md) ────────────────
 *
 * One link at a time, carrying MSP frames on the FFE0 service: FFF1 out,
 * FFF2 in. A PERIPHERAL serves the table in tn_att.c and is dialled; a
 * CENTRAL dials (tn_gatt_dial) and, because the handles are fixed, needs no
 * discovery -- it subscribes to 0x0004 and writes 0x0006. Either side then
 * calls tn_gatt_send() and gets `rx` for what the other side sent, and
 * neither has to know which role it holds.
 *
 * Nothing here runs on the controller's context. Link and data events are
 * parked and delivered from tn_gatt_pump(), which the station calls from a
 * task of its own -- the same shape as the bearers' drain hooks.
 *
 * The ESP32 port serves only (its set becomes connectable; TN_ADV_PROP_
 * CONNECTABLE is the caller's to set). The SoftDevice port dials; serving
 * there is the SoftDevice's own GATTS and is not written yet. */
typedef struct {
    void (*connected)(void *ctx, uint16_t conn, bool as_central);
    void (*disconnected)(void *ctx, uint16_t conn, uint8_t reason);
    void (*rx)(void *ctx, const uint8_t *data, int len);
    void  *ctx;
} tn_gatt_cb_t;

tn_err_t tn_gatt_serve(const tn_gatt_cb_t *cb);   /* accept inbound links */

/* SoftDevice port only: every SoC event the pump drains, for an
 * application that needs one (flash completion). Weak; override it. */
void tn_soc_event(uint32_t evt);
tn_err_t tn_gatt_dial(uint8_t addr_type, const uint8_t addr[6],
                       const tn_gatt_cb_t *cb);    /* SoftDevice port only */
void      tn_gatt_pump(void);
bool      tn_gatt_connected(void);
int       tn_gatt_mtu(void);                       /* bytes per send, now  */
tn_err_t tn_gatt_send(const uint8_t *data, int len);
tn_err_t tn_gatt_disconnect(void);
#endif /* ESP_PLATFORM || TN_PORT_SD */

#ifdef __cplusplus
}
#endif
