/* The fetch state machine with its world faked: what it asks, what it
 * believes, what it fetches, and what it leaves alone. */
#include "xfw.h"
#include "xprs.h"
#include "xprsid.h"
#include "xprssig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void randombytes(unsigned char *p, unsigned long long n)
{ for (unsigned long long i = 0; i < n; i++) p[i] = (unsigned char)(rand() & 0xff); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The world. */
static uint32_t now_s = 1000, epoch = 1792900000;
static bool enabled = true, busy = false;
static const char *chan = "stable";
static uint8_t pub_key[32], station_key[32] = { 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7 };
static uint8_t publisher_priv[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
static char sent_to[16], sent_wire[260]; static int sent_n;
static int packets_sent; static uint8_t last_pkt[600]; static size_t last_pkt_len;
static int sink_begun, sink_finished, sink_aborted; static char sink_version[24]; static size_t sink_size;
static char last_said[200];

static const char *rv(void) { return "0.5.1"; }
static bool pk(uint8_t out[32]) { memcpy(out, pub_key, 32); return true; }
static const char *ch(void) { return chan; }
static bool en(void) { return enabled; }
static uint32_t nowf(void) { return now_s; }
static uint32_t epf(void) { return epoch; }
static bool bz(void) { return busy; }
static bool pick(char *out, size_t cap) { snprintf(out, cap, "X3ARC1"); return true; }
static int sign(char *w, int len, int cap) { return xprsid_sign(w, len, cap, station_key); }
static bool send_to(const char *call, const char *w, int len)
{ snprintf(sent_to, sizeof sent_to, "%s", call); memcpy(sent_wire, w, (size_t)len); sent_wire[len] = 0; sent_n++; return true; }
static bool route(const char *call, uint8_t dest[16], uint8_t pub[64], uint8_t hop[16], bool *have)
{
    if (strcmp(call, "X3ARC1") != 0) return false;
    if (dest) memset(dest, 0xAB, 16);
    if (pub) for (int i = 0; i < 64; i++) pub[i] = (uint8_t)i;
    if (hop) memset(hop, 0, 16);
    if (have) *have = false;
    return true;
}
static bool send_packet(const uint8_t *p, size_t n) { packets_sent++; memcpy(last_pkt, p, n < sizeof last_pkt ? n : sizeof last_pkt); last_pkt_len = n; return true; }
static bool sb(const char *v, size_t size, const uint8_t sha[32]) { (void)sha; sink_begun++; snprintf(sink_version, sizeof sink_version, "%s", v); sink_size = size; return true; }
static bool sw(const uint8_t *d, size_t n) { (void)d; (void)n; return true; }
static bool sf(void) { sink_finished++; return true; }
static void sa(void) { sink_aborted++; }
static void sayf(const char *l) { snprintf(last_said, sizeof last_said, "%s", l); }

static const xfw_cfg_t cfg = {
    .board = "tdeck", .self = "X3SEP2", .running_version = rv, .publisher_key = pk,
    .channel = ch, .enabled = en, .now_s = nowf, .epoch_s = epf, .busy = bz,
    .pick_archiver = pick, .sign = sign, .send_to = send_to, .peer_route = route,
    .send_packet = send_packet, .sink_begin = sb, .sink_write = sw, .sink_finish = sf,
    .sink_abort = sa, .say = sayf,
};

static char pub_call[12];

/* A signed description from the publisher. */
static int describe(char *w, int cap, const char *name, const char *tag, uint32_t size, const uint8_t *priv)
{
    int n = snprintf(w, cap, "t:file f:%s ts:2026-10-25_12:00:00 "
                     "file:89i_2HJJGSRWehTokBmEik9XcylQJfidwoA3FlKEtYY.bin size:%u name:%s tag:%s",
                     pub_call, (unsigned)size, name, tag);
    return xprsid_sign(w, n, cap, priv);
}

static void feed(const char *w, int n) { xfw_on_wire(w, n); xfw_poll(); }

int main(void)
{
    CHECK(xprssig_public_key(publisher_priv, pub_key), "publisher key");
    xfw_publisher_call(pub_key, pub_call);
    CHECK(strlen(pub_call) == 6 && pub_call[0] == 'X' && pub_call[1] == '1', "derived callsign %s", pub_call);
    xfw_start(&cfg);
    char w[260]; int n;

    /* Nothing is asked before the first window; then the publisher's folder is asked for. */
    xfw_poll();
    CHECK(sent_n == 0, "no ask at boot");
    now_s += 400; xfw_poll();
    CHECK(sent_n == 1 && strstr(sent_wire, "cmd:history kind:file only:") && strstr(sent_wire, pub_call), "the folder ask: %s", sent_wire);
    CHECK(strcmp(sent_to, "X3ARC1") == 0, "to the archiver");

    /* Not for us: another board, an older version, the other channel, a stranger's key. */
    n = describe(w, sizeof w, "tdongle-s3-0.9.0.bin", "stable", 1000, publisher_priv); feed(w, n);
    n = describe(w, sizeof w, "tdeck-0.5.0.bin", "stable", 1000, publisher_priv); feed(w, n);
    n = describe(w, sizeof w, "tdeck-0.6.0.bin", "beta", 1000, publisher_priv); feed(w, n);
    n = describe(w, sizeof w, "tdeck-0.9.9.bin", "stable", 1000, station_key); feed(w, n);
    CHECK(sent_n == 1, "nothing fetched for those (%s)", last_said);

    /* The one that counts: newer, our board, our channel, the publisher's key. */
    n = describe(w, sizeof w, "tdeck-0.5.2.bin", "stable", 1460192, publisher_priv); feed(w, n);
    CHECK(sent_n == 2 && strstr(sent_wire, "cmd:file file:89i_2HJJGSRWehTokBmEik9XcylQJfidwoA3FlKEtYY.bin"), "cmd:file asked: %s", sent_wire);
    char st[300]; xfw_state(st, sizeof st);
    CHECK(strstr(st, "state=asked") && strstr(st, "best=0.5.2"), "%s", st);

    /* The holder says 202: the slot opens first (that quiesces the station),
     * the link request goes out five seconds later, to its files destination. */
    xprs_t ask; CHECK(xprs_parse(sent_wire, (int)strlen(sent_wire), &ask), "parse ask");
    char id[XPRS_ID_LEN]; xprs_id(&ask, id);
    n = snprintf(w, sizeof w, "t:result f:X3ARC1 d:X3SEP2 ts:2026-10-25_12:00:01 r:%s code:202", id);
    feed(w, n);
    xfw_state(st, sizeof st);
    CHECK(sink_begun == 1 && packets_sent == 0 && strstr(st, "state=settling"), "slot open, settling: %s", st);
    now_s += 5; xfw_poll();
    CHECK(packets_sent == 1 && (last_pkt[0] & 3) == RNS_PACKET_LINKREQUEST, "LINKREQUEST sent (%d packets, %s)", packets_sent, last_said);
    xfw_state(st, sizeof st);
    CHECK(strstr(st, "state=linking"), "%s", st);

    /* No proof comes: tried again, then given up, and the digest is left alone. */
    now_s += 21; xfw_poll(); now_s += 21; xfw_poll(); now_s += 21; xfw_poll();
    xfw_state(st, sizeof st);
    CHECK(strstr(st, "state=idle") && strstr(st, "fails=1"), "%s", st);
    CHECK(sink_begun == 1 && sink_aborted == 1, "the slot was opened and given back");
    n = describe(w, sizeof w, "tdeck-0.5.2.bin", "stable", 1460192, publisher_priv); feed(w, n);
    CHECK(sent_n == 2, "the failed digest is not asked for again within the hold");

    /* A later, different image is. And a refusal is not retried blindly either. */
    n = snprintf(w, sizeof w, "t:file f:%s ts:2026-10-25_13:00:00 file:%064x size:1460200 name:tdeck-0.5.3.bin tag:stable", pub_call, 0x11);
    n = xprsid_sign(w, n, sizeof w, publisher_priv); feed(w, n);
    CHECK(sent_n == 3 && strstr(sent_wire, "cmd:file"), "the next version is fetched");
    CHECK(xprs_parse(sent_wire, (int)strlen(sent_wire), &ask), "parse"); xprs_id(&ask, id);
    n = snprintf(w, sizeof w, "t:result f:X3ARC1 d:X3SEP2 ts:2026-10-25_13:00:01 r:%s code:404", id); feed(w, n);
    xfw_state(st, sizeof st);
    CHECK(strstr(st, "state=idle") && strstr(st, "fails=2"), "%s", st);

    /* Switched off: the publisher is not even asked. */
    enabled = false; now_s += 7 * 3600; xfw_poll();
    CHECK(sent_n == 3, "off means silent");

    printf("xfw host tests: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
