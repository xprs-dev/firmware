/* ATT server over a compiled-in table, plus L2CAP fixed-channel framing.
 * See tn_att.h.
 *
 * Free of ESP-IDF, heap and tasks, like tn_hci.c and for the same reason:
 * a wrong byte here is not a crash, it is a phone whose service discovery
 * comes back empty, and the desk is where that is cheap to find
 * (test/test_tn_att_host.sh).
 *
 * Layouts: Bluetooth Core 5.x Vol 3 Part F section 3.4 (ATT) and Part A
 * section 3.1 (L2CAP basic frame). Every multi-byte field is little-endian.
 */

#include "tn_att.h"

#include <string.h>

static void w8 (uint8_t **p, uint8_t v)  { *(*p)++ = v; }
static void w16(uint8_t **p, uint16_t v) { w8(p, v & 0xff); w8(p, (v >> 8) & 0xff); }
static uint16_t r16(const uint8_t *p)    { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ── The table ──────────────────────────────────────────────────────────
 *
 * Six attributes, each the 16-bit type it is declared under and the value a
 * read returns. Characteristic declarations carry their own value: the
 * properties byte, the value handle, and the characteristic UUID -- that is
 * what a client parses to learn which handle to write to. */
typedef struct {
    uint16_t handle;
    uint16_t type;
    uint8_t  value[5];
    uint8_t  value_len;
    bool     readable;
} tn_attr_t;

/* Characteristic properties (Vol 3 Part G, 3.3.1.1). */
#define PROP_WRITE_NO_RSP 0x04
#define PROP_WRITE        0x08
#define PROP_NOTIFY       0x10

static const tn_attr_t k_table[] = {
    { TN_H_SVC,         TN_UUID_PRIMARY_SVC,
      { 0xE0, 0xFF }, 2, true },
    { TN_H_NOTIFY_DECL, TN_UUID_CHAR_DECL,
      { PROP_NOTIFY, 0x03, 0x00, 0xF1, 0xFF }, 5, true },
    /* The value itself is never read: it only ever travels as a
     * notification, and a read of it would answer with stale bytes. */
    { TN_H_NOTIFY_VAL,  TN_UUID_MESH_NOTIFY,
      { 0 }, 0, false },
    /* The CCCD's value is per-connection state, filled at read time. */
    { TN_H_NOTIFY_CCCD, TN_UUID_CCCD,
      { 0 }, 2, true },
    { TN_H_WRITE_DECL,  TN_UUID_CHAR_DECL,
      { PROP_WRITE | PROP_WRITE_NO_RSP, 0x06, 0x00, 0xF2, 0xFF }, 5, true },
    { TN_H_WRITE_VAL,   TN_UUID_MESH_WRITE,
      { 0 }, 0, false },
};
#define N_ATTRS (sizeof k_table / sizeof k_table[0])

static const tn_attr_t *find(uint16_t handle)
{
    for (size_t i = 0; i < N_ATTRS; i++)
        if (k_table[i].handle == handle) return &k_table[i];
    return NULL;
}

/* ── Responses ──────────────────────────────────────────────────────── */

static int error_rsp(uint8_t *out, size_t cap, uint8_t req_op,
                     uint16_t handle, uint8_t code)
{
    if (cap < 5) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_ERROR_RSP);
    w8(&p, req_op);
    w16(&p, handle);
    w8(&p, code);
    return 5;
}

/* Copy an attribute's value, which for the CCCD means this connection's
 * subscription state rather than the table's placeholder. */
static int attr_value(const tn_att_t *a, const tn_attr_t *t, uint8_t *dst)
{
    if (t->handle == TN_H_NOTIFY_CCCD) {
        dst[0] = a->notify_on ? 0x01 : 0x00;
        dst[1] = 0x00;
        return 2;
    }
    memcpy(dst, t->value, t->value_len);
    return t->value_len;
}

void tn_att_init(tn_att_t *a)
{
    a->mtu = TN_ATT_MTU_DEFAULT;
    a->notify_on = false;
}

/* Exchange MTU: the answer is OUR ceiling, and the link then runs at the
 * smaller of the two. Sending TN_ATT_MTU_MAX rather than min(theirs, ours)
 * is what the spec says, and it lets a client that asked for less than it
 * can take learn what we could have done. */
static int h_mtu(tn_att_t *a, const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len != 3) return error_rsp(out, cap, TN_ATT_OP_MTU_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t theirs = r16(pdu + 1);
    if (theirs < TN_ATT_MTU_DEFAULT) theirs = TN_ATT_MTU_DEFAULT;
    a->mtu = theirs < TN_ATT_MTU_MAX ? theirs : TN_ATT_MTU_MAX;
    if (cap < 3) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_MTU_RSP);
    w16(&p, TN_ATT_MTU_MAX);
    return 3;
}

/* Find Information: every handle in the range, with its type. Format 0x01
 * throughout, because every type here is a 16-bit UUID. */
static int h_find_info(const tn_att_t *a, const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len != 5) return error_rsp(out, cap, TN_ATT_OP_FIND_INFO_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t lo = r16(pdu + 1), hi = r16(pdu + 3);
    if (lo == 0 || lo > hi) return error_rsp(out, cap, TN_ATT_OP_FIND_INFO_REQ, lo, TN_ATT_ERR_INVALID_HANDLE);
    if (cap < 2) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_FIND_INFO_RSP);
    w8(&p, 0x01);
    size_t room = (cap < a->mtu ? cap : a->mtu) - 2;
    int n = 0;
    for (size_t i = 0; i < N_ATTRS && room >= 4; i++) {
        if (k_table[i].handle < lo || k_table[i].handle > hi) continue;
        w16(&p, k_table[i].handle);
        w16(&p, k_table[i].type);
        room -= 4;
        n++;
    }
    if (!n) return error_rsp(out, cap, TN_ATT_OP_FIND_INFO_REQ, lo, TN_ATT_ERR_NOT_FOUND);
    return (int)(p - out);
}

/* Find By Type Value: "which handle range holds a primary service with
 * this UUID?" -- the one request a client makes when it already knows what
 * it wants. Ours ends at the last handle in the table. */
static int h_find_by_type(const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len < 7) return error_rsp(out, cap, TN_ATT_OP_FIND_BY_TYPE_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t lo = r16(pdu + 1), hi = r16(pdu + 3), type = r16(pdu + 5);
    if (lo == 0 || lo > hi) return error_rsp(out, cap, TN_ATT_OP_FIND_BY_TYPE_REQ, lo, TN_ATT_ERR_INVALID_HANDLE);
    bool match = type == TN_UUID_PRIMARY_SVC && len == 9 &&
                 r16(pdu + 7) == TN_UUID_MESH_SVC &&
                 TN_H_SVC >= lo && TN_H_SVC <= hi;
    if (!match) return error_rsp(out, cap, TN_ATT_OP_FIND_BY_TYPE_REQ, lo, TN_ATT_ERR_NOT_FOUND);
    if (cap < 5) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_FIND_BY_TYPE_RSP);
    w16(&p, TN_H_SVC);
    w16(&p, TN_H_LAST);
    return 5;
}

/* Read By Type: every attribute of one type in the range, handle + value.
 * All the values of a given type here are the same length, so the fixed
 * pair-length format never has to split a response. */
static int h_read_by_type(const tn_att_t *a, const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len != 7) return error_rsp(out, cap, TN_ATT_OP_READ_BY_TYPE_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t lo = r16(pdu + 1), hi = r16(pdu + 3), type = r16(pdu + 5);
    if (lo == 0 || lo > hi) return error_rsp(out, cap, TN_ATT_OP_READ_BY_TYPE_REQ, lo, TN_ATT_ERR_INVALID_HANDLE);
    if (cap < 2) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_READ_BY_TYPE_RSP);
    uint8_t *lenslot = p; w8(&p, 0);
    size_t room = (cap < a->mtu ? cap : a->mtu) - 2;
    int n = 0, pair = 0;
    for (size_t i = 0; i < N_ATTRS; i++) {
        const tn_attr_t *t = &k_table[i];
        if (t->handle < lo || t->handle > hi || t->type != type) continue;
        if (!t->readable)
            return error_rsp(out, cap, TN_ATT_OP_READ_BY_TYPE_REQ, t->handle, TN_ATT_ERR_READ_NOT_PERMITTED);
        uint8_t v[5];
        int vl = attr_value(a, t, v);
        if (n && 2 + vl != pair) break;          /* format change: stop here */
        if (room < (size_t)(2 + vl)) break;
        pair = 2 + vl;
        w16(&p, t->handle);
        memcpy(p, v, vl); p += vl;
        room -= pair;
        n++;
    }
    if (!n) return error_rsp(out, cap, TN_ATT_OP_READ_BY_TYPE_REQ, lo, TN_ATT_ERR_NOT_FOUND);
    *lenslot = (uint8_t)pair;
    return (int)(p - out);
}

/* Read By Group Type: primary-service discovery. One service, so one
 * group: start handle, end handle, UUID. */
static int h_read_by_group(const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len != 7) return error_rsp(out, cap, TN_ATT_OP_READ_BY_GROUP_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t lo = r16(pdu + 1), hi = r16(pdu + 3), type = r16(pdu + 5);
    if (lo == 0 || lo > hi) return error_rsp(out, cap, TN_ATT_OP_READ_BY_GROUP_REQ, lo, TN_ATT_ERR_INVALID_HANDLE);
    if (type != TN_UUID_PRIMARY_SVC)
        return error_rsp(out, cap, TN_ATT_OP_READ_BY_GROUP_REQ, lo, TN_ATT_ERR_NOT_SUPPORTED);
    if (TN_H_SVC < lo || TN_H_SVC > hi)
        return error_rsp(out, cap, TN_ATT_OP_READ_BY_GROUP_REQ, lo, TN_ATT_ERR_NOT_FOUND);
    if (cap < 8) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_READ_BY_GROUP_RSP);
    w8(&p, 6);
    w16(&p, TN_H_SVC);
    w16(&p, TN_H_LAST);
    w16(&p, TN_UUID_MESH_SVC);
    return 8;
}

static int h_read(const tn_att_t *a, const uint8_t *pdu, int len, uint8_t *out, size_t cap)
{
    if (len != 3) return error_rsp(out, cap, TN_ATT_OP_READ_REQ, 0, TN_ATT_ERR_INVALID_LEN);
    uint16_t h = r16(pdu + 1);
    const tn_attr_t *t = find(h);
    if (!t) return error_rsp(out, cap, TN_ATT_OP_READ_REQ, h, TN_ATT_ERR_INVALID_HANDLE);
    if (!t->readable) return error_rsp(out, cap, TN_ATT_OP_READ_REQ, h, TN_ATT_ERR_READ_NOT_PERMITTED);
    if (cap < 1 + 5) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_READ_RSP);
    p += attr_value(a, t, p);
    return (int)(p - out);
}

/* Write Request and Write Command share everything but the answer. */
static int h_write(tn_att_t *a, const uint8_t *pdu, int len, uint8_t *out, size_t cap,
                   tn_att_write_cb_t on_write, void *ctx)
{
    uint8_t op = pdu[0];
    bool want_rsp = op == TN_ATT_OP_WRITE_REQ;
    /* A command gets no error response either -- the spec is explicit --
     * so a bad one is simply dropped. */
    #define REFUSE(h, code) (want_rsp ? error_rsp(out, cap, op, (h), (code)) : 0)

    if (len < 3) return REFUSE(0, TN_ATT_ERR_INVALID_LEN);
    uint16_t h = r16(pdu + 1);
    const uint8_t *val = pdu + 3;
    int vl = len - 3;

    if (h == TN_H_WRITE_VAL) {
        if (vl < 1) return REFUSE(h, TN_ATT_ERR_INVALID_LEN);
        if (on_write) on_write(ctx, val, vl);
    } else if (h == TN_H_NOTIFY_CCCD) {
        if (vl != 2) return REFUSE(h, TN_ATT_ERR_INVALID_LEN);
        a->notify_on = (val[0] & 0x01) != 0;
    } else if (find(h)) {
        return REFUSE(h, TN_ATT_ERR_WRITE_NOT_PERMITTED);
    } else {
        return REFUSE(h, TN_ATT_ERR_INVALID_HANDLE);
    }
    #undef REFUSE

    if (!want_rsp) return 0;
    if (cap < 1) return -1;
    out[0] = TN_ATT_OP_WRITE_RSP;
    return 1;
}

int tn_att_handle(tn_att_t *a, const uint8_t *pdu, int len,
                  uint8_t *out, size_t cap,
                  tn_att_write_cb_t on_write, void *ctx)
{
    if (!a || !pdu || !out || len < 1) return -1;
    switch (pdu[0]) {
    case TN_ATT_OP_MTU_REQ:           return h_mtu(a, pdu, len, out, cap);
    case TN_ATT_OP_FIND_INFO_REQ:     return h_find_info(a, pdu, len, out, cap);
    case TN_ATT_OP_FIND_BY_TYPE_REQ:  return h_find_by_type(pdu, len, out, cap);
    case TN_ATT_OP_READ_BY_TYPE_REQ:  return h_read_by_type(a, pdu, len, out, cap);
    case TN_ATT_OP_READ_BY_GROUP_REQ: return h_read_by_group(pdu, len, out, cap);
    case TN_ATT_OP_READ_REQ:          return h_read(a, pdu, len, out, cap);
    case TN_ATT_OP_WRITE_REQ:
    case TN_ATT_OP_WRITE_CMD:         return h_write(a, pdu, len, out, cap, on_write, ctx);
    default:
        /* Commands (bit 6 set) never get an error response; requests do. */
        if (pdu[0] & 0x40) return 0;
        return error_rsp(out, cap, pdu[0], 0, TN_ATT_ERR_NOT_SUPPORTED);
    }
}

int tn_att_notify(const tn_att_t *a, const uint8_t *val, int len,
                  uint8_t *out, size_t cap)
{
    if (!a || !val || !out || len < 0) return -1;
    if (!a->notify_on) return -1;
    if (len > tn_att_notify_max(a)) return -1;
    if (cap < (size_t)(3 + len)) return -1;
    uint8_t *p = out;
    w8(&p, TN_ATT_OP_NOTIFY);
    w16(&p, TN_H_NOTIFY_VAL);
    memcpy(p, val, len);
    return 3 + len;
}

/* ── L2CAP ─────────────────────────────────────────────────────────── */

int tn_l2cap_wrap(uint8_t *out, size_t cap, uint16_t cid,
                  const uint8_t *payload, int len)
{
    if (!out || !payload || len < 0 || len > 0xffff) return -1;
    if (cap < (size_t)(4 + len)) return -1;
    uint8_t *p = out;
    w16(&p, (uint16_t)len);
    w16(&p, cid);
    memcpy(p, payload, len);
    return 4 + len;
}

int tn_l2cap_unwrap(const uint8_t *frame, int len,
                    uint16_t *cid, const uint8_t **payload)
{
    if (!frame || len < 4) return -1;
    uint16_t plen = r16(frame);
    if (4 + (int)plen > len) return -1;   /* claims more than it carries */
    if (cid) *cid = r16(frame + 2);
    if (payload) *payload = frame + 4;
    return plen;
}
