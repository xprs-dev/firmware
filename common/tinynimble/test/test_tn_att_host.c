/* Are the ATT bytes tn_att.c answers with the bytes a phone expects?
 *
 * A wrong field here is a phone whose service discovery comes back empty, or
 * a write that lands on the wrong handle -- no crash, no error, a session
 * that never starts. So every response is asserted byte-for-byte against
 * Bluetooth Core 5.x Vol 3 Part F, in the order a real client issues them:
 * MTU, then discover the service, its characteristics, its descriptors,
 * subscribe, write, be notified.
 *
 * The decoder is then fed truncated and over-claiming input on purpose. */

#include <stdio.h>
#include <string.h>

#include "tn_att.h"

static int fails;

static void hexdump(const char *tag, const uint8_t *p, int n)
{
    printf("  %-8s", tag);
    for (int i = 0; i < n; i++) printf(" %02x", p[i]);
    printf("\n");
}

static void expect(const char *what, const uint8_t *got, int gotlen,
                   const uint8_t *want, int wantlen)
{
    if (gotlen == wantlen && memcmp(got, want, wantlen) == 0) return;
    printf("FAIL: %s\n", what);
    hexdump("got", got, gotlen > 0 ? gotlen : 0);
    hexdump("want", want, wantlen);
    fails++;
}

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define REQ(a, ...) do { const uint8_t req[] = { __VA_ARGS__ }; \
    n = tn_att_handle(&(a), req, sizeof req, out, sizeof out, on_write, NULL); } while (0)
#define WANT(what, ...) do { const uint8_t want[] = { __VA_ARGS__ }; \
    expect(what, out, n, want, sizeof want); } while (0)

static int writes;
static uint8_t last_write[64];
static int last_write_len;

static void on_write(void *ctx, const uint8_t *d, int len)
{
    (void)ctx;
    writes++;
    last_write_len = len > (int)sizeof last_write ? (int)sizeof last_write : len;
    memcpy(last_write, d, last_write_len);
}

int main(void)
{
    uint8_t out[300];
    int n;
    tn_att_t a;
    tn_att_init(&a);
    CHECK(a.mtu == 23 && !a.notify_on, "fresh link is MTU 23, unsubscribed");

    /* ── 1. Exchange MTU: the phone asks 517, we answer our 247 ──────── */
    REQ(a, 0x02, 0x05, 0x02);
    WANT("mtu rsp", 0x03, 0xF7, 0x00);
    CHECK(a.mtu == 247, "mtu agreed at min(517, 247)");

    /* A client that asks for LESS than we can do gets the link at its
     * figure, but the answer still says what we could have done. */
    tn_att_t b; tn_att_init(&b);
    REQ(b, 0x02, 0x64, 0x00);
    WANT("mtu rsp to a 100-byte client", 0x03, 0xF7, 0x00);
    CHECK(b.mtu == 100, "link runs at the smaller of the two");

    /* ── 2. Primary service discovery ───────────────────────────────── */
    REQ(a, 0x10, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28);
    WANT("read by group: one service, handles 1..6, FFE0",
         0x11, 0x06, 0x01, 0x00, 0x06, 0x00, 0xE0, 0xFF);

    /* The follow-up a client makes past the end: "not found", at its
     * start handle, and NOT an empty response, which hangs iOS. */
    REQ(a, 0x10, 0x07, 0x00, 0xFF, 0xFF, 0x00, 0x28);
    WANT("read by group past the end", 0x01, 0x10, 0x07, 0x00, 0x0A);

    /* Find By Type Value: the phone already knows FFE0. */
    REQ(a, 0x06, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28, 0xE0, 0xFF);
    WANT("find by type value FFE0", 0x07, 0x01, 0x00, 0x06, 0x00);
    REQ(a, 0x06, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28, 0x0F, 0x18);
    WANT("find by type value: a service we do not have", 0x01, 0x06, 0x01, 0x00, 0x0A);

    /* ── 3. Characteristic discovery: Read By Type 0x2803 ────────────── */
    REQ(a, 0x08, 0x01, 0x00, 0x06, 0x00, 0x03, 0x28);
    WANT("read by type 2803: two declarations",
         0x09, 0x07,
         0x02, 0x00,  0x10, 0x03, 0x00, 0xF1, 0xFF,     /* FFF1 notify, value @3 */
         0x05, 0x00,  0x0C, 0x06, 0x00, 0xF2, 0xFF);    /* FFF2 write, value @6 */

    /* ── 4. Descriptor discovery: Find Information 4..4 ──────────────── */
    REQ(a, 0x04, 0x04, 0x00, 0x04, 0x00);
    WANT("find info: the CCCD", 0x05, 0x01, 0x04, 0x00, 0x02, 0x29);

    /* The whole table, as some clients ask for it. */
    REQ(a, 0x04, 0x01, 0x00, 0xFF, 0xFF);
    WANT("find info: everything",
         0x05, 0x01,
         0x01, 0x00, 0x00, 0x28,
         0x02, 0x00, 0x03, 0x28,
         0x03, 0x00, 0xF1, 0xFF,
         0x04, 0x00, 0x02, 0x29,
         0x05, 0x00, 0x03, 0x28,
         0x06, 0x00, 0xF2, 0xFF);

    /* ── 5. Subscribe: write 0x0001 to the CCCD ─────────────────────── */
    CHECK(tn_att_notify(&a, (const uint8_t *)"x", 1, out, sizeof out) == -1,
          "notify refused before subscription");
    REQ(a, 0x12, 0x04, 0x00, 0x01, 0x00);
    WANT("write rsp", 0x13);
    CHECK(a.notify_on, "CCCD bit 0 subscribes");
    REQ(a, 0x0A, 0x04, 0x00);
    WANT("read CCCD back", 0x0B, 0x01, 0x00);

    /* ── 6. The peer writes an MSP frame to FFF2 ───────────────────── */
    writes = 0;
    REQ(a, 0x12, 0x06, 0x00, 0x4D, 0x01, 0x10, 0xAA, 0xBB);
    WANT("write rsp to FFF2", 0x13);
    CHECK(writes == 1 && last_write_len == 5 && last_write[0] == 0x4D,
          "FFF2 write delivered to the callback, payload intact");

    /* Write Command: same delivery, no response at all. */
    REQ(a, 0x52, 0x06, 0x00, 0x4D, 0x01, 0x11);
    CHECK(n == 0 && writes == 2, "write command delivered, answered with nothing");

    /* ── 7. Notify: the frame out on FFF1 ───────────────────────────── */
    n = tn_att_notify(&a, (const uint8_t *)"\x4D\x01\x20", 3, out, sizeof out);
    WANT("notification", 0x1B, 0x03, 0x00, 0x4D, 0x01, 0x20);
    CHECK(tn_att_notify_max(&a) == 244, "244 bytes per notification at MTU 247");
    { uint8_t big[245];
      memset(big, 0, sizeof big);
      CHECK(tn_att_notify(&a, big, 245, out, sizeof out) == -1,
            "a frame over MTU-3 is refused, not truncated");
      CHECK(tn_att_notify(&a, big, 244, out, sizeof out) == 247,
            "exactly MTU-3 fits"); }

    /* Unsubscribe, and the notification path closes again. */
    REQ(a, 0x12, 0x04, 0x00, 0x00, 0x00);
    CHECK(!a.notify_on, "CCCD cleared");
    CHECK(tn_att_notify(&a, (const uint8_t *)"x", 1, out, sizeof out) == -1,
          "notify refused after unsubscribe");

    /* ── 8. Refusals, each with the right code at the right handle ──── */
    REQ(a, 0x0A, 0x03, 0x00);
    WANT("read FFF1 value: not permitted", 0x01, 0x0A, 0x03, 0x00, 0x02);
    REQ(a, 0x0A, 0x09, 0x00);
    WANT("read a handle that does not exist", 0x01, 0x0A, 0x09, 0x00, 0x01);
    REQ(a, 0x12, 0x01, 0x00, 0x00);
    WANT("write the service declaration", 0x01, 0x12, 0x01, 0x00, 0x03);
    REQ(a, 0x12, 0x04, 0x00, 0x01);
    WANT("CCCD write of the wrong length", 0x01, 0x12, 0x04, 0x00, 0x0D);
    REQ(a, 0x12, 0x06, 0x00);
    WANT("empty write to FFF2", 0x01, 0x12, 0x06, 0x00, 0x0D);
    REQ(a, 0x0E, 0x01, 0x00);
    WANT("read multiple: not supported", 0x01, 0x0E, 0x00, 0x00, 0x06);
    REQ(a, 0xD2, 0x01, 0x00);
    CHECK(n == 0, "an unknown COMMAND is dropped silently, per spec");
    REQ(a, 0x04, 0x05, 0x00, 0x02, 0x00);
    WANT("find info with start > end", 0x01, 0x04, 0x05, 0x00, 0x01);
    REQ(a, 0x04, 0x00, 0x00, 0x06, 0x00);
    WANT("find info from handle 0", 0x01, 0x04, 0x00, 0x00, 0x01);
    REQ(a, 0x08, 0x01, 0x00, 0x06, 0x00, 0x05, 0x2A);
    WANT("read by type for a type we have none of", 0x01, 0x08, 0x01, 0x00, 0x0A);

    /* ── 9. MTU-bounded responses ───────────────────────────────────── */
    /* At MTU 23 the whole-table Find Information (2 + 6*4 = 26) has to be
     * cut at 5 entries (22 bytes); the client asks again from handle 6. */
    tn_att_t c; tn_att_init(&c);
    REQ(c, 0x04, 0x01, 0x00, 0xFF, 0xFF);
    CHECK(n == 22, "find info honours MTU 23");
    REQ(c, 0x04, 0x06, 0x00, 0xFF, 0xFF);
    WANT("and the continuation", 0x05, 0x01, 0x06, 0x00, 0xF2, 0xFF);

    /* ── 10. Malformed and truncated input ──────────────────────────── */
    { const uint8_t one[] = { 0x02 };
      n = tn_att_handle(&a, one, 1, out, sizeof out, on_write, NULL);
      WANT("MTU request with no MTU", 0x01, 0x02, 0x00, 0x00, 0x0D); }
    n = tn_att_handle(&a, out, 0, out, sizeof out, on_write, NULL);
    CHECK(n == -1, "empty PDU");
    { uint8_t tiny[2];
      const uint8_t req[] = { 0x10, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x28 };
      n = tn_att_handle(&a, req, sizeof req, tiny, sizeof tiny, on_write, NULL);
      CHECK(n == -1, "output buffer too small says so"); }

    /* ── 11. L2CAP framing ──────────────────────────────────────────── */
    { const uint8_t pdu[] = { 0x13 };
      n = tn_l2cap_wrap(out, sizeof out, TN_L2CAP_CID_ATT, pdu, 1);
      WANT("l2cap frame around a write rsp", 0x01, 0x00, 0x04, 0x00, 0x13);
      uint16_t cid; const uint8_t *pl;
      int m = tn_l2cap_unwrap(out, n, &cid, &pl);
      CHECK(m == 1 && cid == 4 && pl[0] == 0x13, "unwrap round-trips");
      CHECK(tn_l2cap_unwrap(out, 3, &cid, &pl) == -1, "header shorter than 4 refused");
      const uint8_t liar[] = { 0x10, 0x00, 0x04, 0x00, 0x13 };
      CHECK(tn_l2cap_unwrap(liar, sizeof liar, &cid, &pl) == -1,
            "a frame claiming 16 bytes it does not carry is refused"); }

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("OK: tn_att answers every byte the spec asks for\n");
    return 0;
}
