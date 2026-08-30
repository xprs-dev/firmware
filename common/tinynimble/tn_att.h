/*
 * tn_att -- the smallest ATT server that serves the XPRS mesh channel.
 *
 * docs/ble5-gatt.md says why a 1:1 exchange belongs on a connection and not
 * on the three advertising channels everybody shares. This is the piece that
 * makes a connection USABLE: the attribute table a phone discovers, and the
 * handful of ATT PDUs it needs to find it, subscribe, write to it and be
 * notified from it. The Mesh Session Protocol (common/xprs_blemesh) rides on
 * top and does not know this file exists -- its transport contract is one
 * send() and one rx(), and those are the two ends of this.
 *
 * WHAT IT IS NOT. Not a GATT library. The table is compiled in, because the
 * service is known: FFE0, with FFF1 to notify on and FFF2 to be written to --
 * the channel the phones already speak. There is no client, no pairing, no
 * bonding store, no dynamic registration, no long attributes and no
 * signalling channel. That is the whole difference between this and the
 * 37 KB NimBLE host tinynimble exists to not pay for.
 *
 * SHAPE. Same as tn_hci.c: pure functions over caller-owned buffers, no
 * ESP-IDF, no heap, no tasks. A PDU goes in, a PDU comes out, and the bytes
 * of both are asserted on a desk (test/test_tn_att_host.sh) before any of
 * them go near a radio. The port layer owns the connection handle, the ACL
 * framing and the question of which task this runs on.
 *
 *   handle  type    what
 *   0x0001  0x2800  primary service FFE0
 *   0x0002  0x2803  characteristic decl: FFF1, notify
 *   0x0003  FFF1    value (this station -> peer)
 *   0x0004  0x2902  client characteristic configuration for FFF1
 *   0x0005  0x2803  characteristic decl: FFF2, write | write without response
 *   0x0006  FFF2    value (peer -> this station)
 *
 * Layouts are Bluetooth Core 5.x, Vol 3 Part F (ATT) and Part A (L2CAP).
 */
#ifndef TN_ATT_H
#define TN_ATT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest ATT MTU this server will agree to. 247 is what fits one LL
 * data PDU at the 251-byte maximum every BLE 4.2+ controller supports, so a
 * notification of MTU-3 = 244 bytes crosses the air in one packet. The
 * default of 23 is what a link starts at until a client asks. */
#define TN_ATT_MTU_DEFAULT  23
#define TN_ATT_MTU_MAX      247

/* ATT rides L2CAP fixed channel 4. Nothing else on this link does. */
#define TN_L2CAP_CID_ATT    0x0004

/* The UUIDs, 16-bit. */
#define TN_UUID_PRIMARY_SVC 0x2800
#define TN_UUID_CHAR_DECL   0x2803
#define TN_UUID_CCCD        0x2902
#define TN_UUID_MESH_SVC    0xFFE0
#define TN_UUID_MESH_NOTIFY 0xFFF1
#define TN_UUID_MESH_WRITE  0xFFF2

/* Handles. Fixed, and part of the contract with the phone. */
#define TN_H_SVC            0x0001
#define TN_H_NOTIFY_DECL    0x0002
#define TN_H_NOTIFY_VAL     0x0003
#define TN_H_NOTIFY_CCCD    0x0004
#define TN_H_WRITE_DECL     0x0005
#define TN_H_WRITE_VAL      0x0006
#define TN_H_LAST           TN_H_WRITE_VAL

/* ATT opcodes this server understands. Anything else gets an Error Response
 * with "request not supported", which is what the spec says to do and what
 * makes an unsupported client fail loudly instead of hanging. */
#define TN_ATT_OP_ERROR_RSP           0x01
#define TN_ATT_OP_MTU_REQ             0x02
#define TN_ATT_OP_MTU_RSP             0x03
#define TN_ATT_OP_FIND_INFO_REQ       0x04
#define TN_ATT_OP_FIND_INFO_RSP       0x05
#define TN_ATT_OP_FIND_BY_TYPE_REQ    0x06
#define TN_ATT_OP_FIND_BY_TYPE_RSP    0x07
#define TN_ATT_OP_READ_BY_TYPE_REQ    0x08
#define TN_ATT_OP_READ_BY_TYPE_RSP    0x09
#define TN_ATT_OP_READ_REQ            0x0A
#define TN_ATT_OP_READ_RSP            0x0B
#define TN_ATT_OP_READ_BY_GROUP_REQ   0x10
#define TN_ATT_OP_READ_BY_GROUP_RSP   0x11
#define TN_ATT_OP_WRITE_REQ           0x12
#define TN_ATT_OP_WRITE_RSP           0x13
#define TN_ATT_OP_WRITE_CMD           0x52
#define TN_ATT_OP_NOTIFY              0x1B

/* ATT error codes (Vol 3 Part F, 3.4.1.1). */
#define TN_ATT_ERR_INVALID_HANDLE     0x01
#define TN_ATT_ERR_READ_NOT_PERMITTED 0x02
#define TN_ATT_ERR_WRITE_NOT_PERMITTED 0x03
#define TN_ATT_ERR_NOT_SUPPORTED      0x06
#define TN_ATT_ERR_NOT_FOUND          0x0A
#define TN_ATT_ERR_INVALID_LEN        0x0D

/* One connection's ATT state. Two facts, because that is all a fixed table
 * needs to remember about a peer. */
typedef struct {
    uint16_t mtu;         /* agreed MTU; TN_ATT_MTU_DEFAULT until exchanged */
    bool     notify_on;   /* the CCCD's notification bit, as the peer set it */
} tn_att_t;

/* Something the peer wrote to FFF2: one inbound MSP frame. Called from inside
 * tn_att_handle(), on whichever task the port runs that on -- copy and
 * return, the usual rule. */
typedef void (*tn_att_write_cb_t)(void *ctx, const uint8_t *data, int len);

/* A fresh connection: MTU 23, not subscribed. */
void tn_att_init(tn_att_t *a);

/*
 * Handle one inbound ATT PDU (the L2CAP payload, opcode first).
 *
 * Writes the response PDU into `out` and returns its length; returns 0 when
 * the request wants no response (Write Command), and -1 when `out` cannot
 * hold what the spec requires. Never returns -1 for a BAD REQUEST -- those
 * get an Error Response, which is a valid answer with a length.
 *
 * A write to FFF2 is handed to `on_write` before the Write Response is
 * built. A write to the CCCD flips notify_on. Every other handle refuses.
 */
int tn_att_handle(tn_att_t *a, const uint8_t *pdu, int len,
                  uint8_t *out, size_t cap,
                  tn_att_write_cb_t on_write, void *ctx);

/*
 * Build a Handle Value Notification carrying `val` on FFF1.
 *
 * Returns the PDU length, or -1 when the peer has not subscribed (nothing
 * to send it to) or `val` exceeds MTU-3 (the spec truncates; this refuses,
 * because a truncated MSP frame is a corrupt one and the caller can chunk).
 */
int tn_att_notify(const tn_att_t *a, const uint8_t *val, int len,
                  uint8_t *out, size_t cap);

/* How much one notification can carry on this link right now. */
static inline int tn_att_notify_max(const tn_att_t *a) { return a->mtu - 3; }

/* ── L2CAP basic frame, fixed channel ───────────────────────────────────
 *
 * Four bytes -- length, then CID -- in front of the payload. There is no
 * signalling channel and no fragment reassembly here: ATT_MTU is kept at or
 * below the LL data length, so one ATT PDU is one L2CAP frame is one ACL
 * packet, which is the arrangement that lets this stay four bytes. */
int tn_l2cap_wrap(uint8_t *out, size_t cap, uint16_t cid,
                  const uint8_t *payload, int len);

/* Returns payload length and sets *cid and *payload, or -1 when the header
 * claims more than the buffer holds -- a truncated ACL must not become an
 * out-of-range read. */
int tn_l2cap_unwrap(const uint8_t *frame, int len,
                    uint16_t *cid, const uint8_t **payload);

#ifdef __cplusplus
}
#endif
#endif /* TN_ATT_H */
