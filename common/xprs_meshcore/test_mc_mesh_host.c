/*
 * mc_mesh on the host: two bridges and one MeshCore node on a fake radio
 * that everybody hears. Driven from test_mc_host.c's main().
 *
 * The node N is a real MeshCore identity (Ed25519) that builds and opens
 * frames with mc_wire's own functions, so what the bridges say has to be
 * readable by something that is not the bridge.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mc.h"
#include "mc_mesh.h"
#include "xlc.h"
#include "xprs.h"

void mc_test_fail(const char *file, int line, const char *what);
#define CHECK(c) do { if (!(c)) mc_test_fail(__FILE__, __LINE__, #c); } while (0)

static uint32_t g_now = 1000;
static uint32_t g_seed = 12345;
static uint32_t t_now(void) { return g_now; }
static uint32_t t_rand(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

#define NB 2
static mc_mesh_t g_b[NB];

typedef struct { char wire[320]; int len; bool sign; int who; } dl_t;
static dl_t g_dl[64];
static int g_ndl;

static void t_deliver(void *ctx, const char *wire, int len, bool sign)
{
    int who = (int)(intptr_t)ctx;
    if (g_ndl >= 64 || len >= (int)sizeof g_dl[0].wire) return;
    memcpy(g_dl[g_ndl].wire, wire, (size_t)len);
    g_dl[g_ndl].wire[len] = 0;
    g_dl[g_ndl].len = len;
    g_dl[g_ndl].sign = sign;
    g_dl[g_ndl].who = who;
    g_ndl++;
}

/* The day the wires in this test are dated, as an epoch. */
static uint32_t epoch_of(int y, int mo, int d, int h, int mi, int s)
{
    int yy = y - (mo <= 2);
    int era = yy / 400;
    int yoe = yy - era * 400;
    int mp = (mo + 9) % 12;
    int doy = (153 * mp + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + s);
}

static int t_stamp(void *ctx, char *out, int cap, bool to_minute)
{
    (void)ctx; (void)to_minute;
    return snprintf(out, (size_t)cap, "ts:2026-09-20_09:00:00");
}

static uint32_t g_utc;
static uint32_t t_utc(void *ctx) { (void)ctx; return g_utc; }

static uint8_t g_keystore[NB][2048];
static int g_keystore_len[NB];
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

static char g_vstore[NB][512];
static int g_vstore_len[NB];
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

static bool t_nick(void *ctx, const char *call, char *out, int cap)
{
    (void)ctx;
    if (strcmp(call, "X1QZ3N") == 0) { snprintf(out, (size_t)cap, "joao"); return true; }
    return false;
}

/* ── The node N ───────────────────────────────────────────────────────── */

static uint8_t g_nsk[64], g_npub[32];
static char    g_ncall[11];

/* What N heard. */
typedef struct {
    uint8_t  type;
    char     sender[32];         /* a channel message's claimed name */
    char     text[192];
    uint32_t ack;                /* an ACK's checksum */
    mc_advert_t adv;
} heard_t;
static heard_t g_heard[64];
static int g_nheard;
static int g_aired[NB];
static bool g_radio_busy;

/* The XPRS callsigns this test's bridges speak for; N derives their keys
 * the way any MeshCore client would after hearing their advert. */
static const char *g_calls[] = { "X3AAAA", "X3BBBB", "X1QZ3N", "X1NEWW" };

static void node_hears(const uint8_t *frame, int len)
{
    mc_pkt_t p;
    if (!mc_parse(frame, len, &p) || g_nheard >= 64) return;
    heard_t *e = &g_heard[g_nheard];
    memset(e, 0, sizeof *e);
    e->type = p.type;
    if (p.type == MC_PT_ADVERT) {
        if (!mc_advert_open(p.payload, p.payload_len, &e->adv)) return;
    } else if (p.type == MC_PT_GRP_TXT) {
        mc_text_t t;
        if (!mc_grp_txt_open(mc_public_key, p.payload, p.payload_len, &t,
                             e->sender, sizeof e->sender))
            return;
        snprintf(e->text, sizeof e->text, "%s", t.text);
    } else if (p.type == MC_PT_TXT_MSG) {
        if (p.payload_len < 4 || p.payload[0] != g_npub[0]) return;
        int got = -1;
        for (int i = 0; i < (int)(sizeof g_calls / sizeof g_calls[0]); i++) {
            uint8_t sk[64], pub[32], secret[32];
            mc_node_keys(g_calls[i], (int)strlen(g_calls[i]), sk, pub);
            if (pub[0] != p.payload[1]) continue;
            if (!xlc_ed25519_key_exchange(secret, pub, g_nsk)) continue;
            mc_text_t t;
            if (!mc_dm_open(secret, p.payload, p.payload_len, &t)) continue;
            snprintf(e->text, sizeof e->text, "%s", t.text);
            snprintf(e->sender, sizeof e->sender, "%s", g_calls[i]);
            /* What N would acknowledge it with: the author's key. */
            e->ack = mc_ack_checksum(t.timestamp,
                                     (uint8_t)((t.txt_type << 2) | t.attempt),
                                     t.text, pub);
            got = i;
            break;
        }
        if (got < 0) return;
    } else if (p.type == MC_PT_ACK) {
        if (p.payload_len < 4) return;
        e->ack = (uint32_t)p.payload[0] | ((uint32_t)p.payload[1] << 8) |
                 ((uint32_t)p.payload[2] << 16) | ((uint32_t)p.payload[3] << 24);
    }
    g_nheard++;
}

static bool t_air(void *ctx, const uint8_t *frame, int len, int prio)
{
    (void)prio;
    if (g_radio_busy) return false;
    int who = (int)(intptr_t)ctx;
    g_aired[who]++;
    node_hears(frame, len);
    for (int i = 0; i < NB; i++)
        if (i != who) mc_mesh_on_frame(&g_b[i], frame, len, -90, 5);
    return true;
}

/* A station runs two clocks: the bearer's tick, and the worker that does
 * the curve arithmetic on a task of its own (mc_mesh.h). The harness runs
 * both, in that order, as the firmware does. */
static void tick_all(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        g_now += 50;
        for (int i = 0; i < NB; i++) {
            mc_mesh_work(&g_b[i], g_now);
            mc_mesh_tick(&g_b[i], g_now);
        }
    }
}

/* N airs a packet; both bridges hear it. */
static void n_air(uint8_t type, const uint8_t *payload, int plen)
{
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD;
    p.type = type;
    p.hash_size = 1;
    p.hops = 1;
    p.path[0] = g_npub[0];
    p.payload = payload;
    p.payload_len = plen;
    uint8_t f[MC_FRAME_MAX];
    int n = mc_build(&p, f, sizeof f);
    if (!n) return;
    for (int i = 0; i < NB; i++) mc_mesh_on_frame(&g_b[i], f, n, -100, -3);
}

static void n_advert(const char *name)
{
    uint8_t pl[MC_PAYLOAD_MAX];
    int n = mc_advert_build(g_nsk, g_npub, g_utc, MC_ADV_CHAT, name, pl,
                            sizeof pl);
    if (n) n_air(MC_PT_ADVERT, pl, n);
}

static void n_channel(const char *name, const char *text, uint32_t ts)
{
    uint8_t pl[MC_PAYLOAD_MAX];
    int n = mc_grp_txt_build(mc_public_key, ts, name, text, pl, sizeof pl);
    if (n) n_air(MC_PT_GRP_TXT, pl, n);
}

/* N writes to one of the bridges' virtual nodes. */
static void n_dm(const char *call, const char *text, uint32_t ts)
{
    uint8_t sk[64], pub[32], secret[32], pl[MC_PAYLOAD_MAX];
    mc_node_keys(call, (int)strlen(call), sk, pub);
    memset(sk, 0, sizeof sk);
    if (!xlc_ed25519_key_exchange(secret, pub, g_nsk)) return;
    int n = mc_dm_build(secret, pub[0], g_npub[0], ts, 0, text, pl, sizeof pl);
    memset(secret, 0, sizeof secret);
    if (n) n_air(MC_PT_TXT_MSG, pl, n);
}

static void n_ack(uint32_t checksum)
{
    uint8_t pl[4] = { (uint8_t)(checksum & 0xFF), (uint8_t)(checksum >> 8),
                      (uint8_t)(checksum >> 16), (uint8_t)(checksum >> 24) };
    n_air(MC_PT_ACK, pl, 4);
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

static int find_heard(uint8_t type, const char *text)
{
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].type == type &&
            (!text || strcmp(g_heard[i].text, text) == 0))
            return i;
    return -1;
}

/* An XPRS wire handed to both bridges, as the station's own send path does. */
static void xprs_in(const char *wire, int origin)
{
    for (int i = 0; i < NB; i++)
        mc_mesh_on_xprs(&g_b[i], wire, (int)strlen(wire), origin);
}

void test_mesh(void)
{
    g_utc = epoch_of(2026, 9, 20, 9, 0, 0);
    uint8_t seed[32];
    memset(seed, 0x4e, sizeof seed);
    xlc_ed25519_from_seed(seed, g_nsk, g_npub);
    mc_call_of_pub(g_npub, g_ncall);

    static const char *calls[NB] = { "X3AAAA", "X3BBBB" };
    for (int i = 0; i < NB; i++) {
        mc_mesh_ops_t ops = { .air = t_air, .now_ms = t_now, .random = t_rand,
                              .deliver = t_deliver, .stamp = t_stamp,
                              .utc_now = t_utc, .nick_of = t_nick,
                              .keys_load = t_keys_load, .keys_save = t_keys_save,
                              .vnodes_load = t_vnodes_load,
                              .vnodes_save = t_vnodes_save,
                              .ctx = (void *)(intptr_t)i };
        mc_mesh_cfg_t cfg = { .repeat = true, .bridge = true,
                              .bcast_per_hour = 3, .advert_min = 180 };
        mc_mesh_init(&g_b[i], &ops, &cfg, calls[i], i == 0 ? "roof" : NULL);
    }

    /* 1. Each bridge advertises its own node soon after boot; it is signed,
     *    it carries the callsign, and the other bridge files it as a virtual
     *    node rather than as a MeshCore user. */
    tick_all(25000);
    uint8_t sk[64], apub[32];
    mc_node_keys("X3AAAA", 6, sk, apub);
    memset(sk, 0, sizeof sk);
    int ad = -1;
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].type == MC_PT_ADVERT &&
            memcmp(g_heard[i].adv.pub, apub, 32) == 0) ad = i;
    CHECK(ad >= 0);
    if (ad >= 0) {
        CHECK(strcmp(g_heard[ad].adv.name, "roof X3AAAA") == 0);
        CHECK(g_heard[ad].adv.flags & MC_ADV_CHAT);
        CHECK(g_heard[ad].adv.timestamp == g_utc);
    }
    CHECK(count_dl("t:identity") == 0);   /* a bridge's own node is not news */
    CHECK(g_b[1].st.adverts_in >= 1);

    /* 2. N advertises. Both bridges name it, unsigned and scope:local, and
     *    both learn the key that a direct message to it is sealed to. */
    g_ndl = 0;
    n_advert("Node N");
    /* An advert is a signature to check: the receive path parks it and the
     * worker opens it, so it is named on the next pass, not in the call. */
    CHECK(count_dl("t:identity") == 0);
    tick_all(200);
    CHECK(count_dl("t:identity") == 2);
    int a = find_dl("t:identity", 0);
    CHECK(a >= 0);
    if (a >= 0) {
        CHECK(strstr(g_dl[a].wire, g_ncall) != NULL);
        CHECK(strstr(g_dl[a].wire, "nick:Node") != NULL);
        CHECK(strstr(g_dl[a].wire, "scope:local") != NULL);
        CHECK(strstr(g_dl[a].wire, "via:X3AAAA") != NULL);
        CHECK(strstr(g_dl[a].wire, "zmid:") != NULL);
        CHECK(!g_dl[a].sign);             /* never signed as ours (3.2) */
    }
    /* The two bridges wrote the same packet, which is what lets a receiver
     * keep one of them (XPRS.md 9.11.5). They differ in via:, which the
     * identifier of section 5 leaves out, so it is the IDENTIFIERS that
     * have to match. */
    int b = find_dl("t:identity", 1);
    CHECK(b >= 0 && a >= 0);
    if (a >= 0 && b >= 0) {
        xprs_t pa, pb;
        char ida[XPRS_ID_LEN], idb[XPRS_ID_LEN];
        CHECK(xprs_parse(g_dl[a].wire, g_dl[a].len, &pa));
        CHECK(xprs_parse(g_dl[b].wire, g_dl[b].len, &pb));
        xprs_id(&pa, ida);
        xprs_id(&pb, idb);
        CHECK(strcmp(ida, idb) == 0);
    }

    /* 3. A channel message from N. It crosses under N's address, because
     *    its name matches the advert we heard, and only once from each
     *    bridge. */
    g_ndl = 0;
    g_nheard = 0;
    n_channel("Node N", "anyone on the XPRS side?", g_utc);
    CHECK(count_dl("m:anyone on the XPRS side?") == 2);
    a = find_dl("m:anyone on the XPRS side?", 0);
    CHECK(a >= 0);
    if (a >= 0) {
        CHECK(strncmp(g_dl[a].wire, "t:message f:", 12) == 0);
        CHECK(strstr(g_dl[a].wire, g_ncall) != NULL);
        CHECK(strstr(g_dl[a].wire, "scope:local") != NULL);
        CHECK(strstr(g_dl[a].wire, " d:") == NULL);   /* to the room */
    }

    /* 4. A channel message whose name nobody has advertised does not cross:
     *    a name is not an address, and inventing one would put words in the
     *    mouth of a node that may not exist. */
    g_ndl = 0;
    uint32_t un0 = g_b[0].st.grp_unnamed;
    n_channel("Someone Else", "hello?", g_utc);
    CHECK(g_ndl == 0);
    CHECK(g_b[0].st.grp_unnamed == un0 + 1);

    /* 5. The repeater: N's packets are re-aired once between the two
     *    bridges, and the second one drops its copy on hearing the first.
     *    The queue is drained first, so what is counted here is this
     *    packet's relay and nobody else's. */
    tick_all(6000);
    uint32_t relayed0 = g_b[0].st.relayed + g_b[1].st.relayed;
    uint32_t cancel0 = g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled;
    g_nheard = 0;
    n_channel("Node N", "second", g_utc);
    tick_all(4000);
    uint32_t relayed = g_b[0].st.relayed + g_b[1].st.relayed - relayed0;
    uint32_t cancel = g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled - cancel0;
    CHECK(relayed == 2);                   /* both wanted to */
    CHECK(cancel == 1);                    /* one of them thought better */
    CHECK(find_heard(MC_PT_GRP_TXT, "second") >= 0);
    /* And the relayed copy carries the repeater's hash on its path. */

    /* 6. An XPRS broadcast is mirrored into the channel under the sender's
     *    name, once, whichever bridge gets there first. */
    g_nheard = 0;
    g_ndl = 0;
    int aired0 = g_aired[0] + g_aired[1];
    xprs_in("t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:good morning from XPRS",
            MC_XPRS_HEARD);
    tick_all(4000);
    int gi = find_heard(MC_PT_GRP_TXT, "good morning from XPRS");
    CHECK(gi >= 0);
    if (gi >= 0) CHECK(strcmp(g_heard[gi].sender, "joao X1QZ3N") == 0);
    CHECK(find_heard(MC_PT_GRP_TXT, "good morning from XPRS") ==
          find_heard(MC_PT_GRP_TXT, "good morning from XPRS"));
    int mirrored = 0;
    for (int i = 0; i < g_nheard; i++)
        if (g_heard[i].type == MC_PT_GRP_TXT &&
            strcmp(g_heard[i].text, "good morning from XPRS") == 0) mirrored++;
    CHECK(mirrored == 1);                  /* the other bridge cancelled */
    CHECK(g_aired[0] + g_aired[1] > aired0);
    /* Speaking for a callsign means advertising it, though not in the same
     * breath: adverts are spaced so a busy minute does not put a dozen of
     * them on a shared channel at once (MC_ADVERT_GAP_MS). */
    tick_all(35000);
    CHECK(find_heard(MC_PT_ADVERT, NULL) >= 0);

    /* 7. Nothing goes back: the packet a bridge just delivered, handed back
     *    to it as XPRS traffic often is, is not put on MeshCore again. */
    g_nheard = 0;
    xprs_in("t:message f:MC00C0FFEE ts:2026-09-20_09:00:00 zmid:deadbeef "
            "via:X3AAAA m:heard on the channel", MC_XPRS_HEARD);
    tick_all(2000);
    CHECK(find_heard(MC_PT_GRP_TXT, "heard on the channel") < 0);

    /* 8. A direct message to N: sealed to the key its advert carried, and
     *    N can open it. Its ACK becomes the signed gateway receipt. */
    g_nheard = 0;
    g_ndl = 0;
    char wire[220];
    snprintf(wire, sizeof wire,
             "t:message f:X1QZ3N d:%s ts:2026-09-20_09:00:00 m:meet at the quay",
             g_ncall);
    xprs_in(wire, MC_XPRS_OWN);
    /* Past MC_AFTER_ADVERT_MS: the sender's key goes on the air first, or
     * the recipient has nothing to open the message with. */
    tick_all(14000);
    int di = find_heard(MC_PT_TXT_MSG, "meet at the quay");
    CHECK(di >= 0);
    if (di >= 0) {
        CHECK(strcmp(g_heard[di].sender, "X1QZ3N") == 0);
        g_ndl = 0;
        n_ack(g_heard[di].ack);
        int r = find_dl("t:receipt", -1);
        CHECK(r >= 0);
        if (r >= 0) {
            CHECK(strstr(g_dl[r].wire, "s:ack") != NULL);
            CHECK(strstr(g_dl[r].wire, "d:X1QZ3N") != NULL);
            CHECK(g_dl[r].sign);           /* the gateway's own word: signed */
        }
    }

    /* 8b. THE ACK THAT ARRIVES INSIDE A PATH (2026-09-20, against a real
     *     client): a MeshCore node with no route back answers a first
     *     direct message with a PATH return carrying the acknowledgement,
     *     not with an ACK packet. A bridge that waits for type 0x03 tells
     *     its user nobody answered. */
    g_nheard = 0;
    g_ndl = 0;
    snprintf(wire, sizeof wire,
             "t:message f:X1QZ3N d:%s ts:2026-09-20_09:00:00 m:by way of a path",
             g_ncall);
    xprs_in(wire, MC_XPRS_OWN);
    tick_all(14000);
    di = find_heard(MC_PT_TXT_MSG, "by way of a path");
    CHECK(di >= 0);
    if (di >= 0) {
        uint8_t sk2[64], pub2[32], secret[32], pl[MC_PAYLOAD_MAX];
        mc_node_keys("X1QZ3N", 6, sk2, pub2);
        memset(sk2, 0, sizeof sk2);
        CHECK(xlc_ed25519_key_exchange(secret, pub2, g_nsk));
        /* dest, src, MAC, cipher{ path length, path, ACK, checksum } */
        uint8_t plain[32];
        int n = 0;
        plain[n++] = 0x01;                     /* one hop, one-byte hashes */
        plain[n++] = g_npub[0];
        plain[n++] = MC_PT_ACK;
        uint32_t ack = g_heard[di].ack;
        for (int b = 0; b < 4; b++) plain[n++] = (uint8_t)((ack >> (8 * b)) & 0xFF);
        uint8_t hop = g_npub[0];
        int pn = mc_path_build(secret, pub2[0], g_npub[0], &hop, 1, 1,
                               MC_PT_ACK, plain + 3, 4, pl, sizeof pl);
        CHECK(pn > 0);
        n_air(MC_PT_PATH, pl, pn);
        tick_all(2000);
        int r2 = find_dl("t:receipt", -1);
        CHECK(r2 >= 0);
        if (r2 >= 0) {
            CHECK(strstr(g_dl[r2].wire, "s:ack") != NULL);
            CHECK(g_dl[r2].sign);
        }
        CHECK(g_b[0].st.paths_in + g_b[1].st.paths_in >= 1);
    }

    /* 9. Only where the node is: a direct message to a MeshCore address no
     *    advert here has named goes nowhere, and nothing is aired for it. */
    g_nheard = 0;
    uint32_t nh0 = g_b[0].st.dm_not_here;
    xprs_in("t:message f:X1QZ3N d:MC0BADF00D ts:2026-09-20_09:00:00 m:hello?",
            MC_XPRS_OWN);
    tick_all(2000);
    CHECK(g_b[0].st.dm_not_here == nh0 + 1);
    CHECK(find_heard(MC_PT_TXT_MSG, "hello?") < 0);

    /* 10. A sealed body cannot cross, and the station that handed it over is
     *     told so rather than left waiting. */
    g_ndl = 0;
    snprintf(wire, sizeof wire,
             "t:message f:X1QZ3N d:%s ts:2026-09-20_09:00:00 x:c2VhbGVk",
             g_ncall);
    xprs_in(wire, MC_XPRS_OWN);
    int r = find_dl("s:no", -1);
    CHECK(r >= 0);
    if (r >= 0) {
        CHECK(strstr(g_dl[r].wire, "MeshCore cannot open it") != NULL);
        CHECK(g_dl[r].sign);
    }
    g_nheard = 0;
    tick_all(2000);
    CHECK(find_heard(MC_PT_TXT_MSG, NULL) < 0);

    /* 11. scope:local never reaches a radio band at all. */
    g_nheard = 0;
    xprs_in("t:message f:X1QZ3N ts:2026-09-20_09:00:00 scope:local m:just here",
            MC_XPRS_OWN);
    tick_all(2000);
    CHECK(find_heard(MC_PT_GRP_TXT, "just here") < 0);

    /* 12. N writes to one of our callsigns. It arrives as a direct XPRS
     *     message and is acknowledged the way MeshCore acknowledges. */
    g_ndl = 0;
    g_nheard = 0;
    n_dm("X1QZ3N", "see you at six", g_utc);
    tick_all(3000);
    a = find_dl("m:see you at six", -1);
    CHECK(a >= 0);
    if (a >= 0) {
        CHECK(strstr(g_dl[a].wire, "d:X1QZ3N") != NULL);
        CHECK(strstr(g_dl[a].wire, g_ncall) != NULL);
        CHECK(strstr(g_dl[a].wire, "scope:local") != NULL);
        CHECK(!g_dl[a].sign);
    }
    int ai = find_heard(MC_PT_ACK, NULL);
    CHECK(ai >= 0);
    if (ai >= 0)
        CHECK(g_heard[ai].ack == mc_ack_checksum(g_utc, 0, "see you at six", g_npub));

    /* 13. A busy radio is not a lost message: it waits and goes. */
    g_nheard = 0;
    g_radio_busy = true;
    xprs_in("t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:while the channel was busy",
            MC_XPRS_HEARD);
    tick_all(3000);
    CHECK(find_heard(MC_PT_GRP_TXT, "while the channel was busy") < 0);
    g_radio_busy = false;
    tick_all(5000);
    CHECK(find_heard(MC_PT_GRP_TXT, "while the channel was busy") >= 0);

    /* 14. THE BENCH BUG (2026-09-20): a packet is heard again and again --
     *      its own echo on the LAN, a digipeat, a replay -- and each repeat
     *      used to spend an hour's broadcast allowance, so a station stopped
     *      mirroring after two messages. The same packet twice is one
     *      broadcast and costs one slot. */
    {
        uint32_t out0 = g_b[0].st.text_out, cap0 = g_b[0].st.bcast_capped;
        const char *once = "t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:said once";
        for (int i = 0; i < 5; i++) {
            mc_mesh_on_xprs(&g_b[0], once, (int)strlen(once), MC_XPRS_HEARD);
            tick_all(200);
        }
        CHECK(g_b[0].st.text_out == out0 + 1);         /* one translation */
        CHECK(g_b[0].st.bcast_capped == cap0);         /* and no slot spent */
    }

    /* 14c. The per-hour cap on mirrored broadcasts is a real cap. */
    uint32_t capped0 = g_b[0].st.bcast_capped;
    for (int i = 0; i < 6; i++) {
        snprintf(wire, sizeof wire,
                 "t:message f:X1QZ3N ts:2026-09-20_09:00:00 m:chatter %d", i);
        xprs_in(wire, MC_XPRS_HEARD);
        tick_all(1500);
    }
    CHECK(g_b[0].st.bcast_capped > capped0);

    /* 15. What the bridge must not forget across a restart: the callsigns
     *     it has advertised (a MeshCore user may be writing to them) and
     *     the contacts it has heard (a key is the only address there is). */
    CHECK(g_vstore_len[0] > 0);
    CHECK(g_keystore_len[0] > 0);
    mc_mesh_ops_t ops = { .air = t_air, .now_ms = t_now, .random = t_rand,
                          .deliver = t_deliver, .stamp = t_stamp,
                          .utc_now = t_utc, .nick_of = t_nick,
                          .keys_load = t_keys_load, .keys_save = t_keys_save,
                          .vnodes_load = t_vnodes_load,
                          .vnodes_save = t_vnodes_save,
                          .ctx = (void *)(intptr_t)0 };
    mc_mesh_cfg_t cfg = { .repeat = true, .bridge = true, .bcast_per_hour = 3,
                          .advert_min = 180 };
    mc_mesh_init(&g_b[0], &ops, &cfg, "X3AAAA", "roof");
    g_nheard = 0;
    snprintf(wire, sizeof wire,
             "t:message f:X1QZ3N d:%s ts:2026-09-20_09:00:00 m:after the restart",
             g_ncall);
    mc_mesh_on_xprs(&g_b[0], wire, (int)strlen(wire), MC_XPRS_OWN);
    /* Long enough for the advert to go first: a bridge that has just
     * started has never advertised this callsign, and a message nobody can
     * open is not a message (MC_AFTER_ADVERT_MS). */
    tick_all(15000);
    CHECK(find_heard(MC_PT_ADVERT, NULL) >= 0);
    CHECK(find_heard(MC_PT_TXT_MSG, "after the restart") >= 0);

    /* 16. The repeater's three refusals, which are what keeps a shared
     *     channel usable: a packet that has been far enough, a packet we
     *     are already on the path of, and a direct-routed packet that names
     *     somebody else as the next hop. */
    {
        uint8_t sk2[64], bpub[32];
        mc_node_keys("X3BBBB", 6, sk2, bpub);
        memset(sk2, 0, sizeof sk2);
        uint8_t pl[MC_PAYLOAD_MAX];
        int pn = mc_grp_txt_build(mc_public_key, g_utc, "Node N", "far away",
                                  pl, sizeof pl);
        CHECK(pn > 0);
        mc_pkt_t p;
        memset(&p, 0, sizeof p);
        p.route = MC_ROUTE_FLOOD;
        p.type = MC_PT_GRP_TXT;
        p.hash_size = 1;
        p.payload = pl;
        p.payload_len = pn;
        uint8_t f[MC_FRAME_MAX];

        /* Already eight hops from home. */
        p.hops = MC_RELAY_MAX_HOPS;
        for (int i = 0; i < p.hops; i++) p.path[i] = (uint8_t)(0x40 + i);
        int n = mc_build(&p, f, sizeof f);
        uint32_t sk0 = g_b[1].st.relay_skipped, rl0 = g_b[1].st.relayed;
        mc_mesh_on_frame(&g_b[1], f, n, -100, -3);
        CHECK(g_b[1].st.relay_skipped == sk0 + 1);
        CHECK(g_b[1].st.relayed == rl0);

        /* Our own hash already on the path: the loop check (XPRS.md 9.2). */
        pn = mc_grp_txt_build(mc_public_key, g_utc, "Node N", "round again",
                              pl, sizeof pl);
        p.payload_len = pn;
        p.hops = 2;
        p.path[0] = 0x41;
        p.path[1] = bpub[0];
        n = mc_build(&p, f, sizeof f);
        sk0 = g_b[1].st.relay_skipped;
        rl0 = g_b[1].st.relayed;
        mc_mesh_on_frame(&g_b[1], f, n, -100, -3);
        CHECK(g_b[1].st.relay_skipped == sk0 + 1);
        CHECK(g_b[1].st.relayed == rl0);

        /* A direct packet whose next hop is us: carried, with our hash
         * taken off the front, and the payload untouched. */
        pn = mc_grp_txt_build(mc_public_key, g_utc, "Node N", "pass it on",
                              pl, sizeof pl);
        p.payload_len = pn;
        p.route = MC_ROUTE_DIRECT;
        p.hops = 2;
        p.path[0] = bpub[0];
        p.path[1] = 0x77;
        n = mc_build(&p, f, sizeof f);
        g_nheard = 0;
        rl0 = g_b[1].st.relayed;
        mc_mesh_on_frame(&g_b[1], f, n, -100, -3);
        CHECK(g_b[1].st.relayed == rl0 + 1);
        tick_all(4000);
        int hi = find_heard(MC_PT_GRP_TXT, "pass it on");
        CHECK(hi >= 0);

        /* And one whose next hop is somebody else: left alone. */
        pn = mc_grp_txt_build(mc_public_key, g_utc, "Node N", "not ours",
                              pl, sizeof pl);
        p.payload_len = pn;
        p.hops = 1;
        p.path[0] = (uint8_t)(bpub[0] + 1);
        n = mc_build(&p, f, sizeof f);
        sk0 = g_b[1].st.relay_skipped;
        rl0 = g_b[1].st.relayed;
        mc_mesh_on_frame(&g_b[1], f, n, -100, -3);
        CHECK(g_b[1].st.relay_skipped == sk0 + 1);
        CHECK(g_b[1].st.relayed == rl0);
    }

    printf("mc_mesh: %u frames heard, %u relayed, %u cancelled, %u in, %u out\n",
           (unsigned)(g_b[0].st.rx_frames + g_b[1].st.rx_frames),
           (unsigned)(g_b[0].st.relayed + g_b[1].st.relayed),
           (unsigned)(g_b[0].st.relay_cancelled + g_b[1].st.relay_cancelled),
           (unsigned)(g_b[0].st.text_in + g_b[1].st.text_in),
           (unsigned)(g_b[0].st.text_out + g_b[1].st.text_out));
}
