/* Are the bytes tinynimble puts on the wire the bytes the controller expects?
 *
 * This is the one thing that must not drift. A wrong field here is not a
 * compile error and not a crash -- it is a station that advertises nothing, or
 * advertises something no peer parses, and the only symptom is silence on the
 * air. So every command is asserted byte-for-byte against the Bluetooth Core
 * 5.x layout (Vol 4 Part E, 7.8), with the exact parameters the firmware uses
 * in common/xprs_bearer_ble/xprsble.c.
 *
 * The decoder is fed malformed packets on purpose: a controller that truncates
 * a report must not become an out-of-range read. */

#include <stdio.h>
#include <string.h>

#include "tinynimble.h"

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

/* The report the decoder should have found. */
static int seen;
static tn_adv_report_t last;
static uint8_t last_data[64];

static void on_report(const tn_adv_report_t *r, void *ctx)
{
    (void)ctx;
    last = *r;
    if (r->data_len <= sizeof last_data) memcpy(last_data, r->data, r->data_len);
    seen++;
}

int main(void)
{
    uint8_t buf[300];
    int n;

    /* ── the bring-up sequence ─────────────────────────────────────────── */
    n = tn_hci_reset(buf, sizeof buf);
    { const uint8_t want[] = { 0x01, 0x03, 0x0C, 0x00 };
      expect("reset", buf, n, want, sizeof want); }

    n = tn_hci_set_event_mask(buf, sizeof buf,
                              TN_EVENT_MASK_DEFAULT | TN_EVENT_MASK_LE_META);
    { /* 0x20001FFFFFFFFFFF little-endian: default bits 0..44 plus LE Meta (61) */
      const uint8_t want[] = { 0x01, 0x01, 0x0C, 0x08,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x20 };
      expect("set_event_mask", buf, n, want, sizeof want); }

    n = tn_hci_le_set_event_mask(buf, sizeof buf,
                                 TN_LE_EVENT_MASK_DEFAULT | TN_LE_EVENT_MASK_EXT_ADV);
    { /* 0x101F: defaults 0..4 plus Extended Advertising Report (bit 12) */
      const uint8_t want[] = { 0x01, 0x01, 0x20, 0x08,
                               0x1F, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
      expect("le_set_event_mask", buf, n, want, sizeof want); }

    /* ── LE Set Random Address ─────────────────────────────────────────── */
    const uint8_t addr[6] = { 0xC0, 0xDE, 0xCA, 0xFE, 0xBA, 0xBE };
    n = tn_hci_set_random_addr(buf, sizeof buf, addr);
    {
        const uint8_t want[] = { 0x01, 0x05, 0x20, 0x06,
                                 0xC0, 0xDE, 0xCA, 0xFE, 0xBA, 0xBE };
        expect("set_random_addr", buf, n, want, sizeof want);
    }

    /* ── LE Set Advertising Set Random Address ─────────────────────────────
     * The command that is NOT 0x2005. Getting this wrong costs a boot: the
     * controller accepts params and data, then refuses the enable with 0x12. */
    n = tn_hci_adv_set_random_addr(buf, sizeof buf, 0, addr);
    {
        const uint8_t want[] = { 0x01, 0x35, 0x20, 0x07,
                                 0x00,          /* advertising handle        */
                                 0xC0, 0xDE, 0xCA, 0xFE, 0xBA, 0xBE };
        expect("adv_set_random_addr", buf, n, want, sizeof want);
    }

    /* ── LE Set Extended Advertising Parameters ────────────────────────────
     * XPRS broadcast set, verbatim from xprsble.c: non-connectable,
     * non-scannable, non-legacy; 160 ms fixed; 1M both PHYs; sid 0;
     * tx_power 127 (controller max). */
    tn_adv_cfg_t adv = {
        .handle = 0, .props = 0,
        .itvl_min = 0x100, .itvl_max = 0x100,
        .chan_map = 0x07, .own_addr_type = 0x01,
        .tx_power = 127,
        .primary_phy = TN_PHY_1M, .secondary_phy = TN_PHY_1M,
        .sid = 0,
    };
    n = tn_hci_ext_adv_params(buf, sizeof buf, &adv);
    {
        const uint8_t want[] = {
            0x01, 0x36, 0x20, 0x19,     /* H4, opcode 0x2036, 25 params      */
            0x00,                       /* handle                            */
            0x00, 0x00,                 /* props: none of them               */
            0x00, 0x01, 0x00,           /* itvl_min 0x000100, 24-bit LE      */
            0x00, 0x01, 0x00,           /* itvl_max                          */
            0x07,                       /* all three primary channels        */
            0x01,                       /* own address type                  */
            0x00,                       /* peer address type (unused)        */
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* peer address           */
            0x00,                       /* filter policy: allow all          */
            0x7F,                       /* tx power 127                      */
            0x01,                       /* primary PHY 1M                    */
            0x00,                       /* secondary max skip                */
            0x01,                       /* secondary PHY 1M                  */
            0x00,                       /* SID                               */
            0x00,                       /* scan request notifications off    */
        };
        expect("ext_adv_params", buf, n, want, sizeof want);
    }

    /* ── LE Set Extended Advertising Data ──────────────────────────────────
     * An XPRS frame: length, type 0xFF, company 0xFFFF, marker 0x3E,
     * subtype 0x41 (APRS), then payload. */
    const uint8_t ad[] = { 0x07, 0xFF, 0xFF, 0xFF, 0x3E, 0x41, 0x68, 0x69 };
    n = tn_hci_ext_adv_data(buf, sizeof buf, 0, ad, sizeof ad);
    {
        uint8_t want[64];
        int w = 0;
        want[w++] = 0x01; want[w++] = 0x37; want[w++] = 0x20;
        want[w++] = (uint8_t)(4 + sizeof ad);
        want[w++] = 0x00;               /* handle                            */
        want[w++] = 0x03;               /* complete data in one command      */
        want[w++] = 0x01;               /* controller may not fragment       */
        want[w++] = (uint8_t)sizeof ad;
        memcpy(want + w, ad, sizeof ad); w += (int)sizeof ad;
        expect("ext_adv_data", buf, n, want, w);
    }

    /* A 254-byte AD is the documented maximum and must be accepted; 255 must
     * not, because the length field is one byte and a silent truncation would
     * put a malformed AD on the air. */
    {
        uint8_t big[255];
        memset(big, 0xAB, sizeof big);
        CHECK(tn_hci_ext_adv_data(buf, sizeof buf, 0, big, 254) == 4 + 4 + 254,
              "254-byte AD was refused");
        CHECK(tn_hci_ext_adv_data(buf, sizeof buf, 0, big, 255) == -1,
              "255-byte AD was accepted");
    }

    /* ── LE Set Extended Advertising Enable ───────────────────────────── */
    n = tn_hci_ext_adv_enable(buf, sizeof buf, 0, true);
    {
        const uint8_t want[] = { 0x01, 0x39, 0x20, 0x06,
                                 0x01,          /* enable                    */
                                 0x01,          /* one set                   */
                                 0x00,          /* handle                    */
                                 0x00, 0x00,    /* duration: forever         */
                                 0x00 };        /* unlimited events          */
        expect("ext_adv_enable(on)", buf, n, want, sizeof want);
    }
    n = tn_hci_ext_adv_enable(buf, sizeof buf, 0, false);
    CHECK(n == 10 && buf[4] == 0x00, "ext_adv_enable(off) did not clear enable");

    /* ── LE Set Extended Scan Parameters ───────────────────────────────────
     * 0x0060 / 0x0050 passive on 1M -- ~83% duty, the value the firmware ships
     * so WiFi still gets airtime. */
    tn_scan_cfg_t scan = {
        .own_addr_type = 0x01, .passive = 1,
        .itvl = 0x0060, .window = 0x0050, .phy = TN_PHY_1M,
    };
    n = tn_hci_ext_scan_params(buf, sizeof buf, &scan);
    {
        const uint8_t want[] = { 0x01, 0x41, 0x20, 0x08,
                                 0x01,          /* own address type          */
                                 0x00,          /* accept all                */
                                 0x01,          /* PHY bitmap: 1M            */
                                 0x00,          /* passive                   */
                                 0x60, 0x00,    /* interval                  */
                                 0x50, 0x00 };  /* window                    */
        expect("ext_scan_params", buf, n, want, sizeof want);
    }

    /* ── LE Set Extended Scan Enable ───────────────────────────────────────
     * Duplicate filtering MUST default off: the controller dedups by address,
     * so a fixed-address peer would be reported once per boot and the mesh
     * beacon -- whose job is to repeat -- would go silent. */
    n = tn_hci_ext_scan_enable(buf, sizeof buf, true, false);
    {
        const uint8_t want[] = { 0x01, 0x42, 0x20, 0x06,
                                 0x01,          /* enable                    */
                                 0x00,          /* filter duplicates OFF     */
                                 0x00, 0x00,    /* duration                  */
                                 0x00, 0x00 };  /* period                    */
        expect("ext_scan_enable", buf, n, want, sizeof want);
    }
    n = tn_hci_ext_scan_enable(buf, sizeof buf, true, true);
    CHECK(buf[5] == 0x01, "filter_dup=true was not honoured");

    /* ── decoding an extended advertising report ───────────────────────── */
    {
        uint8_t ev[64];
        int e = 0;
        ev[e++] = 0x04;                 /* H4 event                          */
        ev[e++] = 0x3E;                 /* LE Meta                           */
        int plen_at = e++;              /* length, filled below              */
        ev[e++] = 0x0D;                 /* Extended Advertising Report       */
        ev[e++] = 0x01;                 /* one report                        */
        ev[e++] = 0x00; ev[e++] = 0x00; /* event type                        */
        ev[e++] = 0x01;                 /* address type                      */
        const uint8_t peer[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };
        memcpy(ev + e, peer, 6); e += 6;
        ev[e++] = 0x01;                 /* primary PHY                       */
        ev[e++] = 0x01;                 /* secondary PHY                     */
        ev[e++] = 0x00;                 /* SID                               */
        ev[e++] = 0x7F;                 /* TX power                          */
        ev[e++] = (uint8_t)(int8_t)-71; /* RSSI                              */
        ev[e++] = 0x00; ev[e++] = 0x00; /* periodic interval                 */
        ev[e++] = 0x00;                 /* direct address type               */
        memset(ev + e, 0, 6); e += 6;   /* direct address                    */
        ev[e++] = (uint8_t)sizeof ad;   /* data length                       */
        memcpy(ev + e, ad, sizeof ad); e += (int)sizeof ad;
        ev[plen_at] = (uint8_t)(e - 3);

        seen = 0;
        int got = tn_hci_feed_evt(ev, e, on_report, NULL);
        CHECK(got == 1 && seen == 1, "one report was not delivered");
        CHECK(last.rssi == -71, "RSSI decoded wrong");
        CHECK(last.addr_type == 0x01, "address type decoded wrong");
        CHECK(memcmp(last.addr, peer, 6) == 0, "address decoded wrong");
        CHECK(last.data_len == sizeof ad, "data length decoded wrong");
        CHECK(memcmp(last_data, ad, sizeof ad) == 0, "data decoded wrong");

        /* Truncation must be refused, not read past. */
        CHECK(tn_hci_feed_evt(ev, e - 1, on_report, NULL) == -1,
              "a truncated event was accepted");
        /* A report claiming more data than the packet holds. */
        uint8_t bad[64];
        memcpy(bad, ev, e);
        bad[e - (int)sizeof ad - 1] = 0xFF;     /* data_len = 255 */
        CHECK(tn_hci_feed_evt(bad, e, on_report, NULL) == -1,
              "a report claiming data past the packet was accepted");
        /* Not an event packet at all. */
        CHECK(tn_hci_feed_evt((const uint8_t *)"\x01\x02\x03", 3, on_report, NULL) == -1,
              "a command packet was decoded as an event");
        /* An event we do not care about is ignored, not an error. */
        const uint8_t other[] = { 0x04, 0x3E, 0x02, 0x02, 0x00 };
        CHECK(tn_hci_feed_evt(other, sizeof other, on_report, NULL) == 0,
              "an unrelated LE event was not ignored");
    }

    /* ── command results ───────────────────────────────────────────────── */
    {
        const uint8_t cc[] = { 0x04, 0x0E, 0x04, 0x01, 0x36, 0x20, 0x00 };
        uint8_t st = 0xFF;
        CHECK(tn_hci_cmd_result(cc, sizeof cc, TN_OP_EXT_ADV_PARAMS, &st),
              "Command Complete for ext_adv_params not recognised");
        CHECK(st == 0x00, "status decoded wrong");
        CHECK(!tn_hci_cmd_result(cc, sizeof cc, TN_OP_EXT_SCAN_ENABLE, &st),
              "Command Complete matched the wrong opcode");

        const uint8_t cs[] = { 0x04, 0x0F, 0x04, 0x0C, 0x01, 0x39, 0x20 };
        CHECK(tn_hci_cmd_result(cs, sizeof cs, TN_OP_EXT_ADV_ENABLE, &st),
              "Command Status not recognised");
        CHECK(st == 0x0C, "Command Status status decoded wrong");
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("PASS: every HCI command encodes byte-exact, and malformed events are refused\n");
    return 0;
}
