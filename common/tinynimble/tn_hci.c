/* HCI command encoding and event decoding. See tinynimble.h.
 *
 * Deliberately free of ESP-IDF, heap and tasks: these bytes are what the
 * controller is told, and the place to find out they are wrong is a desk, not
 * a roof. test/test_tn_host.sh checks every encoder byte-for-byte and feeds the
 * decoder malformed packets on purpose.
 *
 * Field layouts are Bluetooth Core 5.x, Vol 4 Part E, section 7.8. The two that
 * catch people: extended advertising intervals are 24-bit (not 16 like legacy),
 * and the extended scan parameters are a per-PHY array whose length depends on
 * the PHY bitmap. */

#include "tinynimble.h"

#include <string.h>

/* Little-endian writers. Everything on the HCI wire is little-endian. */
static void w8 (uint8_t **p, uint8_t v)  { *(*p)++ = v; }
static void w16(uint8_t **p, uint16_t v) { w8(p, v & 0xff); w8(p, (v >> 8) & 0xff); }
static void w24(uint8_t **p, uint32_t v) { w16(p, v & 0xffff); w8(p, (v >> 16) & 0xff); }

static void w64(uint8_t **p, uint64_t v)
{
    for (int i = 0; i < 8; i++) w8(p, (uint8_t)((v >> (8 * i)) & 0xff));
}

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Start an H4 command packet; returns the cursor just past the length byte,
 * which is back-filled by fin(). */
static uint8_t *begin(uint8_t *buf, size_t cap, size_t need, uint16_t opcode)
{
    /* 1 indicator + 2 opcode + 1 length + parameters */
    if (cap < 4 + need) return NULL;
    uint8_t *p = buf;
    w8(&p, TN_H4_CMD);
    w16(&p, opcode);
    w8(&p, (uint8_t)need);      /* Parameter_Total_Length */
    return p;
}

static int fin(const uint8_t *buf, const uint8_t *p) { return (int)(p - buf); }

int tn_hci_reset(uint8_t *buf, size_t cap)
{
    if (!buf) return -1;
    uint8_t *p = begin(buf, cap, 0, TN_OP_RESET);
    if (!p) return -1;
    return fin(buf, p);
}

int tn_hci_set_event_mask(uint8_t *buf, size_t cap, uint64_t mask)
{
    if (!buf) return -1;
    uint8_t *p = begin(buf, cap, 8, TN_OP_SET_EVENT_MASK);
    if (!p) return -1;
    w64(&p, mask);
    return fin(buf, p);
}

int tn_hci_le_set_event_mask(uint8_t *buf, size_t cap, uint64_t mask)
{
    if (!buf) return -1;
    uint8_t *p = begin(buf, cap, 8, TN_OP_LE_SET_EVENT_MASK);
    if (!p) return -1;
    w64(&p, mask);
    return fin(buf, p);
}

int tn_hci_set_random_addr(uint8_t *buf, size_t cap, const uint8_t addr[6])
{
    if (!buf || !addr) return -1;
    uint8_t *p = begin(buf, cap, 6, TN_OP_SET_RANDOM_ADDR);
    if (!p) return -1;
    memcpy(p, addr, 6);
    p += 6;
    return fin(buf, p);
}

int tn_hci_adv_set_random_addr(uint8_t *buf, size_t cap, uint8_t handle,
                               const uint8_t addr[6])
{
    if (!buf || !addr) return -1;
    uint8_t *p = begin(buf, cap, 7, TN_OP_ADV_SET_RAND_ADDR);
    if (!p) return -1;
    w8(&p, handle);
    memcpy(p, addr, 6);
    p += 6;
    return fin(buf, p);
}

int tn_hci_ext_adv_params(uint8_t *buf, size_t cap, const tn_adv_cfg_t *c)
{
    if (!buf || !c) return -1;
    uint8_t *p = begin(buf, cap, 25, TN_OP_EXT_ADV_PARAMS);
    if (!p) return -1;

    w8 (&p, c->handle);
    w16(&p, c->props);
    w24(&p, c->itvl_min);          /* 24-bit, unlike legacy advertising */
    w24(&p, c->itvl_max);
    w8 (&p, c->chan_map);
    w8 (&p, c->own_addr_type);
    w8 (&p, 0x00);                 /* Peer_Address_Type, unused undirected  */
    memset(p, 0, 6); p += 6;       /* Peer_Address, likewise                */
    w8 (&p, 0x00);                 /* Advertising_Filter_Policy: allow all  */
    w8 (&p, (uint8_t)c->tx_power);
    w8 (&p, c->primary_phy);
    w8 (&p, 0x00);                 /* Secondary_Advertising_Max_Skip        */
    w8 (&p, c->secondary_phy);
    w8 (&p, c->sid);
    w8 (&p, 0x00);                 /* Scan_Request_Notification_Enable      */
    return fin(buf, p);
}

int tn_hci_ext_adv_data(uint8_t *buf, size_t cap, uint8_t handle,
                        const uint8_t *ad, size_t ad_len)
{
    if (!buf || (!ad && ad_len)) return -1;
    if (ad_len > TN_ADV_DATA_MAX) return -1;

    uint8_t *p = begin(buf, cap, 4 + ad_len, TN_OP_EXT_ADV_DATA);
    if (!p) return -1;

    w8(&p, handle);
    w8(&p, 0x03);   /* Operation: complete data in one command              */
    w8(&p, 0x01);   /* Fragment_Preference: controller may not fragment.
                     * Single AD per frame is the XPRS rule so that phones
                     * with ~247-byte caps hear it whole.                    */
    w8(&p, (uint8_t)ad_len);
    if (ad_len) { memcpy(p, ad, ad_len); p += ad_len; }
    return fin(buf, p);
}

int tn_hci_ext_adv_enable(uint8_t *buf, size_t cap, uint8_t handle, bool on)
{
    if (!buf) return -1;
    /* Enable + Num_Sets, then one set: handle, duration, max events. */
    uint8_t *p = begin(buf, cap, 6, TN_OP_EXT_ADV_ENABLE);
    if (!p) return -1;

    w8 (&p, on ? 0x01 : 0x00);
    w8 (&p, 0x01);          /* Number_of_Sets                               */
    w8 (&p, handle);
    w16(&p, 0x0000);        /* Duration: forever. The caller decides when to
                             * stop, because a payload change is stop/set/start
                             * and a timeout would race it.                  */
    w8 (&p, 0x00);          /* Max_Extended_Advertising_Events: unlimited    */
    return fin(buf, p);
}

int tn_hci_ext_scan_params(uint8_t *buf, size_t cap, const tn_scan_cfg_t *c)
{
    if (!buf || !c) return -1;
    /* Own_addr + policy + phy bitmap, then 5 bytes for the one PHY. */
    uint8_t *p = begin(buf, cap, 3 + 5, TN_OP_EXT_SCAN_PARAMS);
    if (!p) return -1;

    w8 (&p, c->own_addr_type);
    w8 (&p, 0x00);                  /* Scanning_Filter_Policy: accept all    */
    w8 (&p, c->phy);                /* bitmap; 1M only in this firmware      */
    w8 (&p, c->passive ? 0x00 : 0x01);
    w16(&p, c->itvl);
    w16(&p, c->window);
    return fin(buf, p);
}

int tn_hci_ext_scan_enable(uint8_t *buf, size_t cap, bool on, bool filter_dup)
{
    if (!buf) return -1;
    uint8_t *p = begin(buf, cap, 6, TN_OP_EXT_SCAN_ENABLE);
    if (!p) return -1;

    w8 (&p, on ? 0x01 : 0x00);
    /* Duplicate filtering stays OFF. The controller dedups by address, so a
     * fixed-address peer is reported once per boot and then never again --
     * which silently breaks the mesh beacon, whose whole job is to repeat.
     * docs/esp32.md records this as a trap paid for once already. */
    w8 (&p, filter_dup ? 0x01 : 0x00);
    w16(&p, 0x0000);        /* Duration: until told otherwise                */
    w16(&p, 0x0000);        /* Period                                        */
    return fin(buf, p);
}

/* ── events ─────────────────────────────────────────────────────────────── */

#define EVT_CMD_COMPLETE   0x0E
#define EVT_CMD_STATUS     0x0F
#define EVT_LE_META        0x3E
#define SUBEVT_EXT_ADV_RPT 0x0D

/* One extended advertising report is 24 fixed bytes then the data. */
#define RPT_FIXED          24

int tn_hci_feed_evt(const uint8_t *pkt, size_t len, tn_report_cb_t cb, void *ctx)
{
    if (!pkt || len < 3) return -1;
    if (pkt[0] != TN_H4_EVT) return -1;

    uint8_t code  = pkt[1];
    uint8_t plen  = pkt[2];
    if (3 + (size_t)plen > len) return -1;      /* truncated, refuse it */

    if (code != EVT_LE_META || plen < 2) return 0;
    if (pkt[3] != SUBEVT_EXT_ADV_RPT) return 0;

    uint8_t nrep = pkt[4];
    const uint8_t *p   = pkt + 5;
    const uint8_t *end = pkt + 3 + plen;
    int delivered = 0;

    for (uint8_t i = 0; i < nrep; i++) {
        if (p + RPT_FIXED > end) return -1;

        tn_adv_report_t r;
        r.evt_type  = r16(p);
        r.addr_type = p[2];
        memcpy(r.addr, p + 3, 6);
        /* p[9] primary PHY, p[10] secondary PHY, p[11] SID,
         * p[12] TX power, p[13] RSSI, p[14..15] periodic interval,
         * p[16] direct addr type, p[17..22] direct addr, p[23] data length */
        r.sid       = p[11];
        r.tx_power  = (int8_t)p[12];
        r.rssi      = (int8_t)p[13];
        r.data_len  = p[23];
        r.data      = p + RPT_FIXED;

        if (r.data + r.data_len > end) return -1;   /* claimed past the packet */

        if (cb) cb(&r, ctx);
        delivered++;
        p += RPT_FIXED + r.data_len;
    }
    return delivered;
}

bool tn_hci_cmd_result(const uint8_t *pkt, size_t len,
                       uint16_t opcode, uint8_t *status)
{
    if (!pkt || len < 3 || pkt[0] != TN_H4_EVT) return false;
    uint8_t code = pkt[1];
    uint8_t plen = pkt[2];
    if (3 + (size_t)plen > len) return false;

    if (code == EVT_CMD_COMPLETE) {
        /* Num_HCI_Command_Packets, Opcode, Return_Parameters[0] = status */
        if (plen < 4) return false;
        if (r16(pkt + 4) != opcode) return false;
        if (status) *status = pkt[6];
        return true;
    }
    if (code == EVT_CMD_STATUS) {
        /* Status, Num_HCI_Command_Packets, Opcode */
        if (plen < 4) return false;
        if (r16(pkt + 5) != opcode) return false;
        if (status) *status = pkt[3];
        return true;
    }
    return false;
}
