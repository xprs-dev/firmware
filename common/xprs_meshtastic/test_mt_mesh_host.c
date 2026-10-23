/*
 * mt_mesh on the host: two bridges and one Meshtastic node on a fake radio
 * that everybody hears. Driven from test_mt_host.c's main().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mt.h"
#include "mt_mesh.h"
#include "xlc.h"
#include "xprs.h"

void mt_test_fail(const char *file, int line, const char *what);
#define CHECK(c) do { if (!(c)) mt_test_fail(__FILE__, __LINE__, #c); } while (0)

static uint32_t g_now = 1000;
static uint32_t g_seed = 12345;
static uint32_t t_now(void) { return g_now; }
static uint32_t t_rand(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

#define NB 2
static mt_mesh_t g_b[NB];

typedef struct { char wire[300]; int len; bool sign; int who; } dl_t;
static dl_t g_dl[64];
static int g_ndl;

/* What the Meshtastic node N heard: decoded LongFast frames. */
typedef struct { mt_hdr_t h; uint32_t port; char text[256]; uint32_t request_id,
                 reply_id, emoji, bitfield; mt_user_t user; int err; } heard_t;
static heard_t g_heard[64];
static int g_nheard;
static int g_aired[NB];
static bool g_radio_busy;       /* air() refuses while set */

static void t_deliver(void *ctx, const char *wire, int len, bool sign)
{
    int who = (int)(intptr_t)ctx;
    if (g_ndl >= 64) return;
    memcpy(g_dl[g_ndl].wire, wire, (size_t)len);
    g_dl[g_ndl].wire[len] = 0;
    g_dl[g_ndl].len = len;
    g_dl[g_ndl].sign = sign;
    g_dl[g_ndl].who = who;
    g_ndl++;
}

static int t_stamp(void *ctx, char *out, int cap, bool to_minute)
{
    (void)ctx; (void)to_minute;
    return snprintf(out, (size_t)cap, "ts:2026-09-19_12:04:00");
}

static uint8_t g_keystore[2][2048];
static int g_keystore_len[2];
static int t_keys_load(void *ctx, void *buf, int cap)
{
    int who = (int)(intptr_t)ctx;
    int n = g_keystore_len[who] < cap ? g_keystore_len[who] : cap;
    memcpy(buf, g_keystore[who], (size_t)n);
    return n;
}
static void t_keys_save(void *ctx, const void *buf, int len)
{
    int who = (int)(intptr_t)ctx;
    if (len > (int)sizeof g_keystore[0]) len = (int)sizeof g_keystore[0];
    memcpy(g_keystore[who], buf, (size_t)len);
    g_keystore_len[who] = len;
}

static char g_vstore[2][512];
static int g_vstore_len[2];
static int t_vnodes_load(void *ctx, void *buf, int cap)
{
    int who = (int)(intptr_t)ctx;
    int n = g_vstore_len[who] < cap ? g_vstore_len[who] : cap;
    memcpy(buf, g_vstore[who], (size_t)n);
    return n;
}
static void t_vnodes_save(void *ctx, const void *buf, int len)
{
    int who = (int)(intptr_t)ctx;
    if (len > (int)sizeof g_vstore[0]) len = (int)sizeof g_vstore[0];
    memcpy(g_vstore[who], buf, (size_t)len);
    g_vstore_len[who] = len;
}

/* The clock the tests run under: 2026-09-19_12:04:00, the stamp above. The
 * case that has no clock sets 0 and puts it back. */
static uint32_t g_utc = 1789819440u;
static uint32_t t_utc(void *ctx) { (void)ctx; return g_utc; }
static bool g_tz_known;
static int32_t g_tz_off;
static int32_t t_tz(void *ctx, bool *known)
{
    (void)ctx;
    if (known) *known = g_tz_known;
    return g_tz_known ? g_tz_off : 0;
}

static bool t_nick(void *ctx, const char *call, char *out, int cap)
{
    (void)ctx;
    if (strcmp(call, "X1QZ3N") == 0) { snprintf(out, (size_t)cap, "joao"); return true; }
    return false;
}

/* The node N: a key pair of its own, as a real Meshtastic node has. */
static const uint32_t N = 0x12345678;
static uint8_t g_npriv[32], g_npub[32];
static int g_nodeinfo_asks;      /* NodeInfo sent to N with want_response */

/* N decodes whatever is on LongFast, and PKI DMs addressed to it (from the
 * XPRS callsigns this test uses, whose keys are derived). */
static void node_hears(const uint8_t *frame, int len)
{
    mt_hdr_t h;
    if (!mt_hdr_parse(frame, len, &h)) return;
    uint8_t buf[256];
    int n = len - 16;
    if (h.channel == mt_longfast_hash()) {
        memcpy(buf, frame + 16, (size_t)n);
        mt_crypt(mt_default_key, 16, h.from, h.id, buf, n);
    } else if (h.channel == 0 && h.to == N) {
        static const char *calls[] = { "X1QZ3N", "X3AAAA", "X3BBBB", "X1NEWW" };
        n = -1;
        for (int i = 0; i < 4 && n < 0; i++) {
            if (mt_node_of_call(calls[i], (int)strlen(calls[i])) != h.from) continue;
            uint8_t spriv[32], spub[32];
            mt_node_keys(calls[i], (int)strlen(calls[i]), spriv, spub);
            n = mt_pki_decrypt(g_npriv, spub, h.from, h.id, frame + 16, len - 16, buf);
        }
        if (n < 0) return;
    } else {
        return;
    }
    mt_data_t d;
    if (!mt_data_decode(buf, n, &d) || g_nheard >= 64) return;
    heard_t *e = &g_heard[g_nheard++];
    memset(e, 0, sizeof *e);
    e->h = h;
    e->port = d.portnum;
    e->request_id = d.request_id;
    e->reply_id = d.reply_id;
    e->emoji = d.emoji;
    e->bitfield = d.has_bitfield ? d.bitfield : 0;
    if (d.portnum == MT_PORT_TEXT) {
        memcpy(e->text, d.payload, (size_t)d.payload_len);
        e->text[d.payload_len] = 0;
    }
    if (d.portnum == MT_PORT_ROUTING) mt_routing_decode(d.payload, d.payload_len, &e->err);
    if (d.portnum == MT_PORT_NODEINFO) {
        mt_user_decode(d.payload, d.payload_len, &e->user);
        if (h.to == N && d.want_response) g_nodeinfo_asks++;
    }
}

static bool t_air(void *ctx, const uint8_t *frame, int len, int prio)
{
    (void)prio;
    if (g_radio_busy) return false;
    int who = (int)(intptr_t)ctx;
    g_aired[who]++;
    node_hears(frame, len);
    for (int i = 0; i < NB; i++)
        if (i != who) mt_mesh_on_frame(&g_b[i], frame, len, -90, 5);
    return true;
}

/* A case that starts a fresh hour: the bridge's budget and every sender's share. */
static void budget_reset(mt_mesh_t *b)
{
    memset(b->bcast_min, 0, sizeof b->bcast_min);
    for (int i = 0; i < MT_VNODES; i++) memset(b->vnodes[i].sent_ms, 0, sizeof b->vnodes[i].sent_ms);
}

static void tick_all(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        g_now += 50;
        for (int i = 0; i < NB; i++) mt_mesh_tick(&g_b[i], g_now);
    }
}

/* N transmits a LongFast frame; both bridges hear it. */
static uint32_t g_nid = 0x5000;
static uint32_t n_send(uint32_t to, uint32_t port, const void *pl, int pn,
                       bool want_ack, uint32_t request_id, uint32_t reply_id,
                       uint32_t emoji, bool ok_mqtt)
{
    mt_data_t d = { 0 };
    d.portnum = port;
    d.payload = pl;
    d.payload_len = pn;
    d.request_id = request_id;
    d.reply_id = reply_id;
    d.emoji = emoji;
    d.has_bitfield = true;
    d.bitfield = ok_mqtt ? 1 : 0;
    uint8_t f[256];
    mt_hdr_t h = { .to = to, .from = N, .id = ++g_nid, .hop_limit = 3,
                   .hop_start = 3, .want_ack = want_ack,
                   .channel = mt_longfast_hash(), .relay_node = 0x78 };
    mt_hdr_build(&h, f);
    int n = mt_data_encode(&d, f + 16, 239);
    mt_crypt(mt_default_key, 16, N, h.id, f + 16, n);
    for (int i = 0; i < NB; i++) mt_mesh_on_frame(&g_b[i], f, 16 + n, -100, -3);
    return h.id;
}

/* N announces itself, key included. */
static void n_nodeinfo(void)
{
    mt_user_t u = { 0 };
    mt_user_id_of(N, u.id);
    strcpy(u.long_name, "Node N");
    strcpy(u.short_name, "NN");
    u.has_public_key = true;
    memcpy(u.public_key, g_npub, 32);
    uint8_t pl[128];
    int pn = mt_user_encode(&u, pl, sizeof pl);
    n_send(MT_BROADCAST, MT_PORT_NODEINFO, pl, pn, false, 0, 0, 0, true);
}

/* N DMs an XPRS callsign's node, sealed to its derived key. */
static uint32_t n_dm(const char *call, const char *text, bool want_ack)
{
    uint32_t to = mt_node_of_call(call, (int)strlen(call));
    uint8_t xpriv[32], xpub[32];
    mt_node_keys(call, (int)strlen(call), xpriv, xpub);
    mt_data_t d = { 0 };
    d.portnum = MT_PORT_TEXT;
    d.payload = (const uint8_t *)text;
    d.payload_len = (int)strlen(text);
    uint8_t plain[240], f[256];
    int pn = mt_data_encode(&d, plain, sizeof plain);
    mt_hdr_t h = { .to = to, .from = N, .id = ++g_nid, .hop_limit = 3,
                   .hop_start = 3, .want_ack = want_ack, .channel = 0,
                   .relay_node = 0x78 };
    mt_hdr_build(&h, f);
    int n = mt_pki_encrypt(g_npriv, xpub, N, h.id, 0xabcdef01u, plain, pn,
                           f + 16, 239);
    for (int i = 0; i < NB; i++) mt_mesh_on_frame(&g_b[i], f, 16 + n, -100, -3);
    return h.id;
}

static int find_dl(const char *needle, int who)
{
    for (int i = 0; i < g_ndl; i++)
        if (strstr(g_dl[i].wire, needle) && (who < 0 || g_dl[i].who == who))
            return i;
    return -1;
}

static int count_dl(const char *needle)
{
    int n = 0;
    for (int i = 0; i < g_ndl; i++) if (strstr(g_dl[i].wire, needle)) n++;
    return n;
}

static int find_heard_text(const char *t)
{
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].port == MT_PORT_TEXT && strcmp(g_heard[i].text, t) == 0)
            return i;
    return -1;
}

void test_mesh(void)
{
    for (int i = 0; i < 32; i++) g_npriv[i] = (uint8_t)(i + 1);
    xlc_x25519_base(g_npub, g_npriv);
    static const char *calls[NB] = { "X3AAAA", "X3BBBB" };
    for (int i = 0; i < NB; i++) {
        mt_mesh_ops_t ops = { .air = t_air, .now_ms = t_now, .random = t_rand,
                              .deliver = t_deliver, .stamp = t_stamp,
                              .nick_of = t_nick, .keys_load = t_keys_load,
                              .keys_save = t_keys_save, .utc_now = t_utc,
                              .tz_offset = t_tz,
                              .vnodes_load = t_vnodes_load,
                              .vnodes_save = t_vnodes_save,
                              .ctx = (void *)(intptr_t)i };
        mt_mesh_cfg_t cfg = { .repeat = true, .bridge = true,
                              .bcast_per_hour = 3, .nodeinfo_min = 180 };
        mt_mesh_init(&g_b[i], &ops, &cfg, calls[i], i == 0 ? "roof" : NULL);
    }

    /* 1. Each bridge announces its own node soon after boot, and the other
     *    recognises it as a virtual node, not a Meshtastic user. */
    tick_all(25000);
    int ni = -1;
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].port == MT_PORT_NODEINFO &&
            g_heard[i].h.from == mt_node_of_call("X3AAAA", 6)) ni = i;
    CHECK(ni >= 0);
    if (ni >= 0) {
        CHECK(strcmp(g_heard[ni].user.long_name, "roof X3AAAA") == 0);
        CHECK(strcmp(g_heard[ni].user.short_name, "AAAA") == 0);
        CHECK(g_heard[ni].user.hw_model == MT_HW_PRIVATE && g_heard[ni].user.has_public_key);
        uint8_t kpriv[32], kpub[32];
        mt_node_keys("X3AAAA", 6, kpriv, kpub);
        CHECK(memcmp(g_heard[ni].user.public_key, kpub, 32) == 0);
    }
    CHECK(count_dl("t:identity") == 0);     /* bridges' own nodes: no identity */

    /* 2. N says hello on LongFast, OK to MQTT. Both bridges translate it to
     *    the same packet (same identifier), and relay once between them. */
    g_ndl = 0;
    uint32_t r0 = g_b[0].st.relayed + g_b[1].st.relayed;
    uint32_t c0 = g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled;
    const char *hello = "hello xprs";
    uint32_t hid = n_send(MT_BROADCAST, MT_PORT_TEXT, hello, (int)strlen(hello),
                          false, 0, 0, 0, true);
    CHECK(g_ndl == 2);
    int a = find_dl("m:hello xprs", 0), b = find_dl("m:hello xprs", 1);
    CHECK(a >= 0 && b >= 0);
    if (a >= 0 && b >= 0) {
        CHECK(strstr(g_dl[a].wire, "t:message f:MT12345678 ts:2026-09-19_12:04:00") == g_dl[a].wire);
        CHECK(strstr(g_dl[a].wire, " scope:regional "));
        CHECK(strstr(g_dl[a].wire, "via:X3AAAA") && strstr(g_dl[b].wire, "via:X3BBBB"));
        CHECK(!g_dl[a].sign);
        char ia[7], ib[7];
        CHECK(xprs_id_of(g_dl[a].wire, g_dl[a].len, ia) &&
              xprs_id_of(g_dl[b].wire, g_dl[b].len, ib) && strcmp(ia, ib) == 0);
    }
    int before = g_aired[0] + g_aired[1];
    tick_all(12000);
    CHECK(g_aired[0] + g_aired[1] - before == 1);   /* one relay; the other cancelled */
    CHECK(g_b[0].st.relayed + g_b[1].st.relayed - r0 == 2);
    CHECK(g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled - c0 == 1);
    (void)hid;

    /* 3. With or without OK to MQTT, the channel's talk is the Regional
     *    room's: regional never reaches the internet, so the consent is
     *    honoured either way (9.11.5). */
    g_ndl = 0;
    n_send(MT_BROADCAST, MT_PORT_TEXT, "private", 7, false, 0, 0, 0, false);
    CHECK(find_dl("scope:regional", 0) >= 0 && find_dl("m:private", 0) >= 0);
    tick_all(12000);

    /* 4. An XPRS broadcast reaches both bridges; one frame goes out, from
     *    the author's virtual node, preceded by that node's NodeInfo. */
    g_nheard = 0;
    const char *xb = "t:message f:X1QZ3N ts:2026-09-19_12:05:00 scope:regional m:hi from xprs";
    for (int i = 0; i < NB; i++) mt_mesh_on_xprs(&g_b[i], xb, (int)strlen(xb), MT_XPRS_HEARD);
    tick_all(6000);
    int ht = find_heard_text("hi from xprs");
    CHECK(ht >= 0);
    int texts = 0, infos = 0;
    for (int i = 0; i < g_nheard; i++) {
        if (g_heard[i].port == MT_PORT_TEXT) texts++;
        if (g_heard[i].port == MT_PORT_NODEINFO &&
            strcmp(g_heard[i].user.long_name, "joao X1QZ3N") == 0) infos++;
    }
    CHECK(texts == 1 && infos == 1);
    uint32_t vq = mt_node_of_call("X1QZ3N", 6);
    uint32_t mirror_id = ht >= 0 ? g_heard[ht].h.id : 0;
    if (ht >= 0) CHECK(g_heard[ht].h.from == vq && g_heard[ht].h.to == MT_BROADCAST);

    /* ...and a copy of the XPRS packet heard again is not mirrored twice. */
    g_nheard = 0;
    mt_mesh_on_xprs(&g_b[0], xb, (int)strlen(xb), MT_XPRS_HEARD);
    tick_all(3000);
    CHECK(find_heard_text("hi from xprs") < 0);

    /* 5. N taps a thumb on it: an XPRS like of the right packet. */
    g_ndl = 0;
    static const uint8_t thumb[] = { 0xF0, 0x9F, 0x91, 0x8D };
    n_send(MT_BROADCAST, MT_PORT_TEXT, thumb, 4, false, 0, mirror_id, 1, true);
    char xid[7];
    xprs_id_of(xb, (int)strlen(xb), xid);
    char want[96];
    snprintf(want, sizeof want, "t:reaction f:MT12345678 ts:2026-09-19_12:04:00 scope:regional r:%s", xid);
    CHECK(find_dl(want, 0) >= 0);
    CHECK(find_dl("add:like", 0) >= 0);
    tick_all(12000);

    /* 6. N DMs X1QZ3N's virtual node, sealed to its derived key. The bridges
     *    have not heard N's key yet: they ask for it, and read the retry. */
    g_ndl = 0;
    g_nheard = 0;
    g_nodeinfo_asks = 0;
    uint32_t dm = n_dm("X1QZ3N", "are you there", true);
    CHECK(find_dl("t:message f:MT12345678 d:X1QZ3N", 0) < 0);
    tick_all(3000);
    int naks = 0;
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].port == MT_PORT_ROUTING && g_heard[i].request_id == dm &&
            g_heard[i].err == MT_ERR_PKI_UNKNOWN_PUBKEY) naks++;
    CHECK(naks == 1);                    /* one, though both bridges saw it */
    n_nodeinfo();
    g_ndl = 0;
    g_nheard = 0;
    dm = n_dm("X1QZ3N", "are you there", true);
    CHECK(find_dl("t:message f:MT12345678 d:X1QZ3N", 0) >= 0);
    CHECK(find_dl("m:are you there", 1) >= 0);
    CHECK(g_b[0].st.pki_in >= 1);
    tick_all(6000);
    int acks = 0;
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].port == MT_PORT_ROUTING && g_heard[i].request_id == dm &&
            g_heard[i].h.to == N && g_heard[i].h.from == vq) acks++;
    CHECK(acks == 1);

    /* 7. X1QZ3N answers N; N's ack comes back as a signed gateway receipt. */
    g_ndl = 0;
    g_nheard = 0;
    const char *xd = "t:message f:X1QZ3N d:MT12345678 ts:2026-09-19_12:06:00 m:yes, here";
    mt_mesh_on_xprs(&g_b[0], xd, (int)strlen(xd), MT_XPRS_OWN);
    tick_all(4000);
    int dh = find_heard_text("yes, here");
    CHECK(dh >= 0);
    if (dh >= 0) {
        CHECK(g_heard[dh].h.to == N && g_heard[dh].h.want_ack);
        /* A DM's ack wants an ack itself, as the firmware sends it. */
        uint8_t ack[2] = { 0x18, 0x00 };
        g_nheard = 0;
        uint32_t aid = n_send(vq, MT_PORT_ROUTING, ack, 2, true, g_heard[dh].h.id,
                              0, 0, false);
        tick_all(3000);
        int ackack = 0;
        for (int i = 0; i < g_nheard; i++)
            if (g_heard[i].port == MT_PORT_ROUTING && g_heard[i].request_id == aid &&
                g_heard[i].h.to == N && g_heard[i].h.hop_limit == 0) ackack++;
        CHECK(ackack == 1);
        char dxid[7];
        xprs_id_of(xd, (int)strlen(xd), dxid);
        char rw[96];
        snprintf(rw, sizeof rw, "t:receipt f:X3AAAA d:X1QZ3N ts:2026-09-19_12:04:00 r:%s s:ack", dxid);
        int r = find_dl(rw, 0);
        CHECK(r >= 0 && g_dl[r].sign);
    }

    /* 8. A DM never acked is tried three times, then waits for N. */
    g_ndl = 0;
    g_nheard = 0;
    const char *xd2 = "t:message f:X1QZ3N d:MT12345678 ts:2026-09-19_12:07:00 m:ping";
    mt_mesh_on_xprs(&g_b[0], xd2, (int)strlen(xd2), MT_XPRS_OWN);
    tick_all(130000);
    /* Counted by who aired it: bridge B, a flood node like any other,
     * relays A's DM once as well, and that is correct. */
    int tries = 0;
    for (int i = 0; i < g_nheard; i++)
        if (strcmp(g_heard[i].text, "ping") == 0 &&
            g_heard[i].h.relay_node == (uint8_t)g_b[0].self) tries++;
    CHECK(tries == 3);
    g_nheard = 0;
    n_send(MT_BROADCAST, MT_PORT_TEXT, "back", 4, false, 0, 0, 0, true);  /* N is heard */
    tick_all(4000);
    CHECK(find_heard_text("ping") >= 0);

    /* 9. Sealed to MT: refused out loud, signed, and nothing aired. */
    g_ndl = 0;
    g_nheard = 0;
    const char *xs = "t:message f:X1QZ3N d:MT12345678 ts:2026-09-19_12:08:00 x:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    mt_mesh_on_xprs(&g_b[0], xs, (int)strlen(xs), MT_XPRS_OWN);
    int rs = find_dl("s:no m:sealed", 0);
    CHECK(rs >= 0 && g_dl[rs].sign);
    tick_all(3000);
    CHECK(g_nheard == 0);

    /* 10. scope:local never goes onto the radio; the cap holds. */
    const char *xl = "t:message f:X1QZ3N ts:2026-09-19_12:09:00 scope:local m:just here";
    mt_mesh_on_xprs(&g_b[0], xl, (int)strlen(xl), MT_XPRS_HEARD);
    tick_all(3000);
    CHECK(find_heard_text("just here") < 0);
    uint32_t capped = g_b[0].st.bcast_capped;
    const char *c1 = "t:message f:X1QZ3N ts:2026-09-19_12:10:01 scope:regional m:one";
    const char *c2 = "t:message f:X1QZ3N ts:2026-09-19_12:10:02 scope:regional m:two";
    const char *c3 = "t:message f:X1QZ3N ts:2026-09-19_12:10:03 scope:regional m:three";
    mt_mesh_on_xprs(&g_b[0], c1, (int)strlen(c1), MT_XPRS_HEARD);
    mt_mesh_on_xprs(&g_b[0], c2, (int)strlen(c2), MT_XPRS_HEARD);
    mt_mesh_on_xprs(&g_b[0], c3, (int)strlen(c3), MT_XPRS_HEARD);
    CHECK(g_b[0].st.bcast_capped > capped);   /* 3 an hour: hi, one, two */
    tick_all(6000);

    /* 11. What came from the mesh is never sent back to it. */
    g_nheard = 0;
    const char *back = "t:message f:MT12345678 ts:2026-09-19_12:04:00 zmid:1234567800005001 via:X3BBBB m:echo";
    mt_mesh_on_xprs(&g_b[0], back, (int)strlen(back), MT_XPRS_HEARD);
    tick_all(3000);
    CHECK(g_nheard == 0);

    /* 12. A long Meshtastic text splits into parts, every one a packet. */
    g_ndl = 0;
    char longt[230];
    memset(longt, 'w', sizeof longt);
    for (int i = 9; i < (int)sizeof longt; i += 10) longt[i] = ' ';
    n_send(MT_BROADCAST, MT_PORT_TEXT, longt, (int)sizeof longt, false, 0, 0, 0, true);
    CHECK(count_dl(" n:1/2") == 2 && count_dl(" n:2/2") == 2);
    for (int i = 0; i < g_ndl; i++) CHECK(g_dl[i].len <= XPRS_MAX_WIRE);

    /* 13. The radio refusing (CAD busy, budget spent) delays, never loses. */
    g_nheard = 0;
    g_radio_busy = true;
    const char *xw = "t:message f:X1QZ3N d:MT12345678 ts:2026-09-19_12:11:00 m:wait";
    mt_mesh_on_xprs(&g_b[1], xw, (int)strlen(xw), MT_XPRS_OWN);
    tick_all(3000);
    CHECK(find_heard_text("wait") < 0);
    g_radio_busy = false;
    tick_all(3000);
    CHECK(find_heard_text("wait") >= 0);

    /* 15. A restarted bridge still knows N's key (saved, then loaded). */
    tick_all(61000);
    CHECK(g_keystore_len[1] > 0);
    {
        mt_mesh_ops_t ops = g_b[1].ops;
        mt_mesh_cfg_t cfg = g_b[1].cfg;
        mt_mesh_init(&g_b[1], &ops, &cfg, "X3BBBB", NULL);
        mt_vnode_t *keep = NULL;
        (void)keep;
        const char *xr = "t:message f:X1QZ3N ts:2026-09-19_12:13:00 scope:local m:hi";
        mt_mesh_on_xprs(&g_b[1], xr, (int)strlen(xr), MT_XPRS_HEARD);  /* learns X1QZ3N */
        g_ndl = 0;
        n_dm("X1QZ3N", "after a restart", true);
        CHECK(find_dl("m:after a restart", 1) >= 0);
    }

    /* 16. N's NodeInfo sent reliably to one of our nodes (what the firmware
     *     does after a PKI_UNKNOWN_PUBKEY) is acknowledged. */
    {
        g_nheard = 0;
        mt_user_t u = { 0 };
        mt_user_id_of(N, u.id);
        strcpy(u.long_name, "Node N");
        u.has_public_key = true;
        memcpy(u.public_key, g_npub, 32);
        uint8_t pl[128];
        int pn = mt_user_encode(&u, pl, sizeof pl);
        uint32_t nid = n_send(vq, MT_PORT_NODEINFO, pl, pn, true, 0, 0, 0, false);
        tick_all(4000);
        int acked = 0;
        for (int i = 0; i < g_nheard; i++)
            if (g_heard[i].port == MT_PORT_ROUTING && g_heard[i].request_id == nid &&
                g_heard[i].err == 0) acked++;
        CHECK(acked == 1);
    }

    /* 17. Old news does not cross: a broadcast an hour old is not mirrored,
     *     one a minute old is. 2026-09-19_12:04:00 is 1789819440. */
    {
        g_utc = 1789819440u + 3600u;
        budget_reset(&g_b[0]);   /* cap: case 10 */
        g_nheard = 0;
        uint32_t stale0 = g_b[0].st.stale;
        const char *old1 = "t:message f:X1QZ3N ts:2026-09-19_12:04:00 scope:regional m:an hour ago";
        const char *new1 = "t:message f:X1QZ3N ts:2026-09-19_13:03:00 scope:regional m:a minute ago";
        mt_mesh_on_xprs(&g_b[0], old1, (int)strlen(old1), MT_XPRS_HEARD);
        mt_mesh_on_xprs(&g_b[0], new1, (int)strlen(new1), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("an hour ago") < 0);
        CHECK(find_heard_text("a minute ago") >= 0);
        CHECK(g_b[0].st.stale == stale0 + 1);
        g_utc = 1789819440u;
    }

    /* 18. No clock, nothing heard crosses: the bearers replay old packets
     *     and the bridge could not tell them from news. The station's own
     *     user's words still go: they are new by definition. */
    {
        g_utc = 0;
        budget_reset(&g_b[0]);
        g_nheard = 0;
        uint32_t nc = g_b[0].st.no_clock;
        const char *h1 = "t:message f:X1QZ3N ts:2026-09-19_13:05:00 scope:regional m:heard without a clock";
        const char *o1 = "t:message f:X1QZ3N ts:2026-09-19_13:05:30 scope:regional m:typed without a clock";
        mt_mesh_on_xprs(&g_b[0], h1, (int)strlen(h1), MT_XPRS_HEARD);
        mt_mesh_on_xprs(&g_b[0], o1, (int)strlen(o1), MT_XPRS_OWN);
        tick_all(6000);
        CHECK(find_heard_text("heard without a clock") < 0);
        CHECK(find_heard_text("typed without a clock") >= 0);
        CHECK(g_b[0].st.no_clock == nc + 1);
        g_utc = 1789819440u;
    }

    /* 19. Just after boot nothing heard crosses either, for the same
     *     reason; twenty seconds later it does. */
    {
        budget_reset(&g_b[0]);
        g_nheard = 0;
        g_b[0].boot_ms = g_now;                  /* as if it had just started */
        g_utc = 1789819440u + 3660u;             /* 13:05:00 */
        uint32_t st = g_b[0].st.settling;
        const char *s1 = "t:message f:X1QZ3N ts:2026-09-19_13:05:00 scope:regional m:too soon";
        mt_mesh_on_xprs(&g_b[0], s1, (int)strlen(s1), MT_XPRS_HEARD);
        CHECK(g_b[0].st.settling == st + 1);
        tick_all(21000);
        const char *s2 = "t:message f:X1QZ3N ts:2026-09-19_13:05:10 scope:regional m:settled";
        mt_mesh_on_xprs(&g_b[0], s2, (int)strlen(s2), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("too soon") < 0);
        CHECK(find_heard_text("settled") >= 0);
        g_utc = 1789819440u;
    }

    /* 20. A text that travelled says when it was written, because the
     *     Meshtastic frame cannot: ten minutes old crosses with its time in
     *     front -- UTC when the station knows no zone, local when it does,
     *     and with the day when that is not the reader's today. */
    {
        budget_reset(&g_b[0]);
        g_nheard = 0;
        g_utc = 1789819440u + 3600u;             /* 13:04:00 UTC */
        g_tz_known = false;
        uint32_t dated0 = g_b[0].st.dated;
        const char *t1 = "t:message f:X1QZ3N ts:2026-09-19_12:54:00 scope:regional m:ten minutes ago";
        mt_mesh_on_xprs(&g_b[0], t1, (int)strlen(t1), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("[12:54Z] ten minutes ago") >= 0);
        g_tz_known = true;
        g_tz_off = 3600;                         /* +01:00 */
        budget_reset(&g_b[0]);
        const char *t2 = "t:message f:X1QZ3N ts:2026-09-19_12:55:00 scope:regional m:nine minutes ago";
        mt_mesh_on_xprs(&g_b[0], t2, (int)strlen(t2), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("[13:55] nine minutes ago") >= 0);
        /* 23:10 UTC is ten past midnight in the station's zone: a text
         * from 22:55 UTC was written yesterday, local time. */
        g_utc = 1789819440u + 39960u;            /* 23:10:00 UTC */
        budget_reset(&g_b[0]);
        const char *t3 = "t:message f:X1QZ3N ts:2026-09-19_22:55:00 scope:regional m:before midnight";
        mt_mesh_on_xprs(&g_b[0], t3, (int)strlen(t3), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("[09-19 23:55] before midnight") >= 0);
        CHECK(g_b[0].st.dated == dated0 + 3);
        /* A minute old carries nothing. */
        budget_reset(&g_b[0]);
        const char *t4 = "t:message f:X1QZ3N ts:2026-09-19_23:09:00 scope:regional m:just now";
        mt_mesh_on_xprs(&g_b[0], t4, (int)strlen(t4), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("just now") >= 0);
        CHECK(g_b[0].st.dated == dated0 + 3);
        g_utc = 1789819440u;
        g_tz_known = false;
    }

    /* 14. A DM to a node whose key nobody has heard: asked for, and held. */
    g_nheard = 0;
    uint32_t asks = g_b[0].st.key_asks;
    const char *xu = "t:message f:X1QZ3N d:MT0BADCAFE ts:2026-09-19_12:12:00 m:who are you";
    mt_mesh_on_xprs(&g_b[0], xu, (int)strlen(xu), MT_XPRS_OWN);
    tick_all(3000);
    CHECK(g_b[0].st.key_asks == asks + 1);
    CHECK(find_heard_text("who are you") < 0);

    /* 18. A DM from a node never announced goes out AFTER that node's
     *     NodeInfo: a DM that overtakes its key cannot be opened, and the
     *     recipient then drops every repeat of its id (2026-09-19). And when
     *     the recipient says PKI_UNKNOWN_PUBKEY anyway, it is told the key
     *     and the DM goes again under a new id; its ack is the receipt. */
    {
        uint32_t vn = mt_node_of_call("X1NEWW", 6);
        g_nheard = 0;
        g_ndl = 0;
        const char *xn = "t:message f:X1NEWW d:MT12345678 ts:2026-09-19_12:14:00 m:first words";
        mt_mesh_on_xprs(&g_b[0], xn, (int)strlen(xn), MT_XPRS_OWN);
        tick_all(12000);
        int info = -1, text = -1;
        for (int i = 0; i < g_nheard; i++) {
            if (g_heard[i].h.from != vn) continue;
            if (g_heard[i].port == MT_PORT_NODEINFO && info < 0) info = i;
            if (g_heard[i].port == MT_PORT_TEXT && text < 0) text = i;
        }
        CHECK(info >= 0 && text >= 0 && info < text);

        g_nheard = 0;
        g_ndl = 0;
        const char *xk = "t:message f:X1NEWW d:MT12345678 ts:2026-09-19_12:15:00 m:rekey me";
        mt_mesh_on_xprs(&g_b[0], xk, (int)strlen(xk), MT_XPRS_OWN);
        tick_all(4000);
        int first = find_heard_text("rekey me");
        CHECK(first >= 0);
        uint32_t old_id = first >= 0 ? g_heard[first].h.id : 0;
        uint8_t nak[8];
        int nn = mt_routing_encode(nak, sizeof nak, MT_ERR_PKI_UNKNOWN_PUBKEY);
        g_nheard = 0;
        n_send(vn, MT_PORT_ROUTING, nak, nn, false, old_id, 0, 0, false);
        tick_all(12000);
        int told = 0, again = -1;
        for (int i = 0; i < g_nheard; i++) {
            if (g_heard[i].h.from != vn) continue;
            if (g_heard[i].port == MT_PORT_NODEINFO && g_heard[i].h.to == N &&
                g_heard[i].h.relay_node == (uint8_t)g_b[0].self) told++;   /* B relays it too */
            if (g_heard[i].port == MT_PORT_TEXT && !strcmp(g_heard[i].text, "rekey me") &&
                again < 0) again = i;
        }
        CHECK(told == 1);
        CHECK(again >= 0 && g_heard[again].h.id != old_id);
        CHECK(find_dl("s:no", 0) < 0);            /* not a failure yet */
        CHECK(g_b[0].st.dm_rekeyed == 1);
        if (again >= 0) {
            uint8_t ack[2] = { 0x18, 0x00 };
            n_send(vn, MT_PORT_ROUTING, ack, 2, false, g_heard[again].h.id, 0, 0, false);
            char kxid[7];
            xprs_id_of(xk, (int)strlen(xk), kxid);
            char rw[48];
            snprintf(rw, sizeof rw, "r:%s s:ack", kxid);
            CHECK(find_dl(rw, 0) >= 0);
        }
    }

    /* 19. A bridge that restarts still speaks for the XPRS callsigns it
     *     spoke for: a Meshtastic user's DM to one is translated at once,
     *     not flooded past until that station happens to speak again
     *     (2026-09-19, the phone app's DM to the desktop after a reflash). */
    {
        tick_all(61000);                       /* X1NEWW, case 18, saved */
        CHECK(g_vstore_len[0] >= MT_CALL_LEN);
        mt_mesh_ops_t ops = g_b[0].ops;
        mt_mesh_cfg_t cfg = g_b[0].cfg;
        mt_mesh_init(&g_b[0], &ops, &cfg, "X3AAAA", "roof");
        g_ndl = 0;
        n_dm("X1NEWW", "still there after a restart?", true);
        CHECK(find_dl("d:X1NEWW", 0) >= 0);
        CHECK(find_dl("m:still there after a restart?", 0) >= 0);

        /* Only what was announced on Meshtastic is written: a station merely
         * heard (a beacon) costs no flash write, however many there are. */
        const char *bz = "t:observation f:X3ZZZZ link:ble peers:1";
        mt_mesh_on_xprs(&g_b[0], bz, (int)strlen(bz), MT_XPRS_HEARD);
        tick_all(61000);
        bool saved_z = false;
        for (int i = 0; i + MT_CALL_LEN <= g_vstore_len[0]; i += MT_CALL_LEN)
            if (!strcmp(g_vstore[0] + i, "X3ZZZZ")) saved_z = true;
        CHECK(!saved_z);
        CHECK(!g_b[0].vnodes_dirty);
    }

    /* 20. A DM heard from elsewhere (a reply that came over the internet)
     *     goes onto this radio only for a node this bridge has heard: the
     *     bridge near the node delivers, every other one stays quiet. One
     *     handed over by the station's own user may still ask for a key. */
    {
        uint32_t asks = g_b[1].st.key_asks, left = g_b[1].st.dm_not_here;
        g_nheard = 0;
        const char *far = "t:message f:X1QZ3N d:MT0BADCAFE ts:2026-09-19_12:16:00 m:from far away";
        mt_mesh_on_xprs(&g_b[1], far, (int)strlen(far), MT_XPRS_HEARD);
        tick_all(4000);
        CHECK(g_b[1].st.dm_not_here == left + 1);
        CHECK(g_b[1].st.key_asks == asks);        /* no NodeInfo request aired */
        CHECK(g_nheard == 0);

        const char *near = "t:message f:X1QZ3N d:MT12345678 ts:2026-09-19_12:16:30 m:to a node we hear";
        mt_mesh_on_xprs(&g_b[1], near, (int)strlen(near), MT_XPRS_HEARD);
        tick_all(15000);                    /* behind its NodeInfo */
        CHECK(find_heard_text("to a node we hear") >= 0);
        CHECK(g_b[1].st.dm_not_here == left + 1);
    }

    /* 21. Only the Regional room goes onto the channel (9.11.3): a global
     *     broadcast (the default) and a country-scoped one stay off it,
     *     counted; a regional one crosses, and never marked OK to MQTT. */
    {
        budget_reset(&g_b[0]);
        g_nheard = 0;
        uint32_t nr = g_b[0].st.not_regional;
        const char *gl = "t:message f:X1QZ3N ts:2026-09-19_12:04:30 m:to the whole world";
        const char *pt = "t:message f:X1QZ3N ts:2026-09-19_12:04:31 scope:PT m:to the country";
        const char *rg = "t:message f:X1QZ3N ts:2026-09-19_12:04:32 scope:regional m:to the valley";
        mt_mesh_on_xprs(&g_b[0], gl, (int)strlen(gl), MT_XPRS_HEARD);
        mt_mesh_on_xprs(&g_b[0], pt, (int)strlen(pt), MT_XPRS_HEARD);
        mt_mesh_on_xprs(&g_b[0], rg, (int)strlen(rg), MT_XPRS_HEARD);
        tick_all(6000);
        CHECK(find_heard_text("to the whole world") < 0);
        CHECK(find_heard_text("to the country") < 0);
        int rv = find_heard_text("to the valley");
        CHECK(rv >= 0);
        if (rv >= 0) CHECK(!(g_heard[rv].bitfield & MT_BITFIELD_OK_TO_MQTT));
        CHECK(g_b[0].st.not_regional == nr + 2);
    }

    /* 22. One sender's share: four an hour, the fifth waits for the hour
     *     to turn, while the bridge's own budget still has room. */
    {
        uint16_t hour = g_b[0].cfg.bcast_per_hour;
        g_b[0].cfg.bcast_per_hour = 12;
        budget_reset(&g_b[0]);
        g_nheard = 0;
        uint32_t sc0 = g_b[0].st.sender_capped;
        char w[120];
        for (int i = 1; i <= 5; i++) {
            snprintf(w, sizeof w, "t:message f:X1QZ3N ts:2026-09-19_12:04:%02d scope:regional m:share %d", 40 + i, i);
            mt_mesh_on_xprs(&g_b[0], w, (int)strlen(w), MT_XPRS_HEARD);
        }
        tick_all(12000);
        CHECK(find_heard_text("share 4") >= 0);
        CHECK(find_heard_text("share 5") < 0);
        CHECK(g_b[0].st.sender_capped == sc0 + 1);
        g_b[0].cfg.bcast_per_hour = hour;
    }

    printf("mt_mesh: %u frames heard, %u relayed, %u cancelled, %u texts in, %u out\n",
           (unsigned)(g_b[0].st.rx_frames + g_b[1].st.rx_frames),
           (unsigned)(g_b[0].st.relayed + g_b[1].st.relayed),
           (unsigned)(g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled),
           (unsigned)(g_b[0].st.text_in + g_b[1].st.text_in),
           (unsigned)(g_b[0].st.text_out + g_b[1].st.text_out));
}
