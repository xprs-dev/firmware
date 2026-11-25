/* Host test for xb_qos (QoS.md) and the bearer under it. The rules are the
 * Dart twin's (core test/xprs_qos_test.dart), checked the same way.
 * Run: ./test_xbqos_host.sh */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "xprsbearer.h"
#include "xb_airtime.h"
#include "xb_qos.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Meshtastic EU LongFast, XPRS in Meshtastic frames (16-byte header, a wire
 * over 220 bytes in two). */
static const xb_lora_air_t k_lf = { .bw_hz = 250000, .sf = 11, .cr = 1,
                                    .preamble = 16, .crc = true };
static uint32_t lf_ms(int len, void *c)
{
    (void)c;
    if (len <= 220) return xb_lora_airtime_ms(&k_lf, len + 16);
    return xb_lora_airtime_ms(&k_lf, 236) + xb_lora_airtime_ms(&k_lf, len - 220 + 16);
}

#define T0     "2026-11-25_12:00:00"
#define T0_S   1795608000u          /* 2026-11-25 12:00:00 UTC */

static xq_t g_q;
static bool g_here, g_cheaper;
static uint32_t g_epoch;
static bool h_here(const char *c) { (void)c; return g_here; }
static bool h_cheaper(const char *c) { (void)c; return g_cheaper; }
static uint32_t h_epoch(void) { return g_epoch; }

static xq_ctx_t ctx(uint32_t now, bool own)
{
    xq_ctx_t c = { .now_ms = now, .epoch_s = T0_S + now / 1000,
                   .budget_ms = 360000, .own = own };
    return c;
}

static void gate(void)
{
    xq_init(&g_q, &XQ_PROFILE_LORA, XPRS_REACH_REGIONAL);
    xq_hooks_t h = { .heard_here = h_here, .reached_cheaper = h_cheaper };
    xq_set_hooks(&g_q, &h);
    g_here = true; g_cheaper = false;
}

static xq_decision_t admit(const char *w, const xq_ctx_t *c)
{
    return xq_admit(&g_q, w, (int)strlen(w), NULL, c, true);
}

static void aired(const char *w, xq_class_t cls, uint32_t now)
{
    xq_aired(&g_q, w, (int)strlen(w), NULL, cls, 0,
             lf_ms((int)strlen(w), NULL), 0, 360000, now);
}

/* ── the pieces ─────────────────────────────────────────────────────────── */

static void test_fields_and_time(void)
{
    const char *w = "t:message f:x1abc-7 d:X3SEP2 ts:" T0 " m:ts:1999 f:NOPE";
    int vl;
    const char *v = xq_field(w, (int)strlen(w), "ts", &vl);
    CHECK(v && vl == 19, "ts field");
    CHECK(xq_ts_epoch(v, vl) == T0_S, "epoch %u", xq_ts_epoch(v, vl));
    CHECK(xq_ts_epoch("1970-01-01_00:00:10", 19) == 10, "epoch 10");
    CHECK(xq_ts_epoch("2026-11-25 12:00:00", 19) == 0, "not a ts");
    v = xq_field(w, (int)strlen(w), "f", &vl);
    CHECK(v && vl == 7, "f: is before m:, never inside it");
    CHECK(xq_call_hash("x1abc-7", 7) == xq_call_hash("X1ABC", 5), "base, any case");
    CHECK(!xq_field(w, (int)strlen(w), "urg", &vl), "no urg");
}

static void test_classes(void)
{
    xq_ctx_t c = ctx(0, false), o = ctx(0, true);
#define CLS(w, cx) xq_classify(w, (int)strlen(w), &cx)
    CHECK(CLS("t:sos f:X1A m:help", o) == XQ_LIFE, "sos is life, even ours");
    CHECK(CLS("t:warning f:X1A", c) == XQ_LIFE, "warning");
    CHECK(CLS("t:message f:X1A m:hi", o) == XQ_OWNER, "own");
    CHECK(CLS("t:message f:X1A m:hi", c) == XQ_CONV, "message");
    CHECK(CLS("t:status f:X1A m:out", c) == XQ_CONV, "status is a person's words");
    CHECK(CLS("t:receipt f:X1A r:abcdef", c) == XQ_CONV, "receipt");
    CHECK(CLS("t:command f:X1A d:X3B cmd:history", c) == XQ_TXN, "command");
    CHECK(CLS("t:identity f:X1A k:abc", c) == XQ_KEYS, "identity");
    CHECK(CLS("t:observation f:X3A", c) == XQ_PRESENCE, "observation");
    CHECK(CLS("t:service f:X3A", c) == XQ_PRESENCE, "service");
    CHECK(CLS("t:mailbox f:X3A", c) == XQ_BULK, "mailbox");
#undef CLS
    const char *u = "t:message f:X1A urg:urgent m:x";
    CHECK(xq_urg_rank(u, (int)strlen(u), false) == 0, "a known urgent");
    CHECK(xq_urg_rank(u, (int)strlen(u), true) == 1, "a stranger's urgent is high");
}

static void test_scope_binds_everything(void)
{
    gate();
    xq_ctx_t c = ctx(1000, false);
    CHECK(admit("t:sos f:X1A scope:local m:help", &c).shed == XQ_SCOPE,
          "a local sos is for the room");
    CHECK(admit("t:message f:X1A scope:regional m:hi", &c).shed == XQ_OK, "regional");
    CHECK(admit("t:message f:X1A scope:galaxy m:hi", &c).shed == XQ_SCOPE,
          "a scope word nobody knows");
    xq_ctx_t o = ctx(1000, true);
    CHECK(admit("t:message f:X1A scope:local m:hi", &o).shed == XQ_SCOPE,
          "the owner's local too");
}

static void test_repeat_interval(void)
{
    gate();
    const char *obs = "t:observation f:X3NEAR pos:1,2";
    xq_ctx_t c = ctx(0, false);
    CHECK(admit(obs, &c).shed == XQ_OK, "first beacon");
    aired(obs, XQ_PRESENCE, 0);
    c = ctx(10 * 60000, false);
    CHECK(admit(obs, &c).shed == XQ_REPEAT, "10 min later on LoRa");
    const char *obs2 = "t:observation f:X3NEAR pos:1,3";
    c = ctx(29 * 60000, false);
    CHECK(admit(obs2, &c).shed == XQ_OK, "29 min: inside 5%% drift of 30");
    c = ctx(40 * 60000, false);
    c.stations = 9;
    CHECK(admit(obs2, &c).shed == XQ_REPEAT, "crowded: an hour");
    /* An identity with a new key is news. */
    const char *id1 = "t:identity f:X1P k:AAAA";
    const char *id2 = "t:identity f:X1P k:BBBB";
    c = ctx(0, false);
    aired(id1, XQ_KEYS, 0);
    c = ctx(60000, false);
    CHECK(admit(id1, &c).shed == XQ_REPEAT, "same key");
    CHECK(admit(id2, &c).shed == XQ_OK, "new key goes at once");
    /* Our own beacon is a beacon. */
    xq_ctx_t o = ctx(0, true);
    const char *mine = "t:observation f:X3ME pos:1,2";
    aired(mine, XQ_OWNER, 0);
    o = ctx(5 * 60000, true);
    CHECK(admit(mine, &o).shed == XQ_REPEAT, "own beacon after 5 min on LoRa");
    /* On a short bearer, five minutes. */
    xq_t s;
    xq_init(&s, &XQ_PROFILE_SHORT, XPRS_REACH_LOCAL);
    xq_ctx_t sc = { .now_ms = 0 };
    xq_aired(&s, obs, (int)strlen(obs), NULL, XQ_PRESENCE, 0, 0, 0, 0, 0);
    sc.now_ms = 4 * 60000;
    CHECK(xq_admit(&s, obs, (int)strlen(obs), NULL, &sc, true).shed == XQ_REPEAT, "4 min");
    sc.now_ms = 5 * 60000;
    CHECK(xq_admit(&s, obs, (int)strlen(obs), NULL, &sc, true).shed == XQ_OK, "5 min");
}

static void test_destinations(void)
{
    gate();
    xq_ctx_t c = ctx(1000, false);
    g_cheaper = true;
    CHECK(admit("t:message f:X1A d:X1B m:hi", &c).shed == XQ_REACHED,
          "reached on Bluetooth already");
    g_cheaper = false;
    g_here = false;
    CHECK(admit("t:command f:X1A d:X3FAR cmd:history", &c).shed == XQ_NOT_HERE,
          "a command for nobody on LoRa");
    CHECK(admit("t:command f:X1A cmd:history", &c).shed == XQ_NOT_HERE,
          "a command for nobody at all");
    g_here = true;
    CHECK(admit("t:command f:X1A d:X3FAR ts:" T0 " cmd:history", &c).shed == XQ_OK,
          "a command for somebody heard here");
    /* Unmetered: no transaction rule. */
    c.budget_ms = 0;
    g_here = false;
    CHECK(admit("t:command f:X1A d:X3FAR cmd:history", &c).shed == XQ_OK, "unmetered");
}

static void test_aired_and_old_news(void)
{
    gate();
    const char *m = "t:message f:X1A ts:" T0 " m:hello";
    xq_ctx_t c = ctx(0, false);
    CHECK(admit(m, &c).shed == XQ_OK, "fresh");
    aired(m, XQ_CONV, 0);
    c = ctx(20 * 60000, false);
    CHECK(admit(m, &c).shed == XQ_AIRED, "again within 30 min");
    /* Old news: 2 h old. */
    const char *old = "t:message f:X1A ts:2026-11-25_10:00:00 m:old";
    c = ctx(1000, false);
    xq_decision_t d = admit(old, &c);
    CHECK(d.shed == XQ_OK && d.cls == XQ_BULK, "old news is bulk, carried on spare");
    c.spent_ms = 200000;            /* 44% free */
    d = admit(old, &c);
    CHECK(d.shed == XQ_OLD_NEWS, "no spare capacity: shed");
    c.spent_ms = 0;
    c.stations = 9;
    CHECK(admit(old, &c).shed == XQ_OLD_NEWS, "crowded: shed");
    /* Clockless: now is the newest ts: heard. */
    gate();
    xq_ctx_t nc = { .now_ms = 1000, .budget_ms = 360000 };
    const char *recent = "t:message f:X1Z ts:" T0 " m:x";
    xq_note(&g_q, recent, (int)strlen(recent), 1000);
    CHECK(admit(old, &nc).cls == XQ_BULK, "clockless: old by the newest ts:");
}

static void test_reserve_tiers(void)
{
    gate();
    xq_ctx_t c = ctx(0, false);
    c.spent_ms = 323000;            /* 89.7% */
    CHECK(xq_may_air(&g_q, XQ_CONV, 0, 2000, &c) == XQ_BUDGET, "third party stops at 90%%");
    CHECK(xq_may_air(&g_q, XQ_OWNER, 0, 2000, &c) == XQ_OK, "owner goes");
    c.spent_ms = 352000;
    CHECK(xq_may_air(&g_q, XQ_OWNER, 0, 2000, &c) == XQ_BUDGET, "owner stops at 98%%");
    CHECK(xq_may_air(&g_q, XQ_LIFE, 0, 2000, &c) == XQ_OK, "sos goes");
    c.spent_ms = 359000;
    CHECK(xq_may_air(&g_q, XQ_LIFE, 0, 2000, &c) == XQ_BUDGET, "even sos: not past 100%%");
    c.budget_ms = 0;
    CHECK(xq_may_air(&g_q, XQ_BULK, 0, 99999, &c) == XQ_OK, "unmetered");
}

static void test_pace(void)
{
    gate();
    xq_ctx_t c = ctx(0, false);
    /* 36 s of third-party airtime fits in six minutes, not 37. */
    for (int i = 0; i < 18; i++)
        xq_aired(&g_q, NULL, 0, NULL, XQ_CONV, 0, 2000, 0, 360000, (uint32_t)i * 1000);
    c.now_ms = 20000;
    CHECK(xq_may_air(&g_q, XQ_CONV, 0, 1000, &c) == XQ_BUDGET, "a tenth in six minutes");
    CHECK(xq_may_air(&g_q, XQ_OWNER, 0, 1000, &c) == XQ_OK, "the owner is not paced");
    c.now_ms = 380000;
    CHECK(xq_may_air(&g_q, XQ_CONV, 0, 1000, &c) == XQ_OK, "window moved on");
    /* Background gets half. */
    gate();
    for (int i = 0; i < 9; i++)
        xq_aired(&g_q, NULL, 0, NULL, XQ_PRESENCE, 0, 2000, 0, 360000, (uint32_t)i * 1000);
    c.now_ms = 20000;
    CHECK(xq_may_air(&g_q, XQ_PRESENCE, 0, 1000, &c) == XQ_BUDGET, "presence: half the pace");
    CHECK(xq_may_air(&g_q, XQ_CONV, 0, 1000, &c) == XQ_OK, "the other half is conversation's");
}

static void test_fair_share(void)
{
    gate();
    uint32_t a = xq_call_hash("X1LOUD", 6), b = xq_call_hash("X1QUIET", 7);
    /* X1LOUD has spent 85 s this hour, spaced past the pace. */
    for (int i = 0; i < 17; i++)
        xq_aired(&g_q, NULL, 0, NULL, XQ_CONV, a, 5000, 0, 360000, (uint32_t)i * 200000);
    uint32_t now = 17u * 200000 - 1;
    /* Equal share with the minimum divisor of 4: 324/4 = 81 s. */
    uint32_t cap = xq_fair_cap(&g_q, a, 360000, now);
    CHECK(cap >= 80000 && cap <= 110000, "cap %u", cap);
    xq_ctx_t c = ctx(now, false);
    c.spent_ms = 85000;
    CHECK(xq_may_air(&g_q, XQ_CONV, a, 30000, &c) == XQ_FAIR, "over the share");
    CHECK(xq_may_air(&g_q, XQ_CONV, b, 2000, &c) == XQ_OK, "the quiet one goes");
}

/* ── the bearer under the gate ──────────────────────────────────────────── */

static uint32_t g_now = 1000;
static int g_air_n;
static char g_aired[16][XB_WIRE_MAX + 1];
static bool f_air(void *c, const char *w, int n)
{
    (void)c;
    if (g_air_n < 16) { memcpy(g_aired[g_air_n], w, (size_t)n); g_aired[g_air_n][n] = 0; }
    g_air_n++;
    return true;
}
static uint32_t f_now(void) { return g_now; }
static uint32_t f_rand(void) { return 0; }
static const xb_ops_t k_ops = { .air = f_air, .now_ms = f_now, .random = f_rand,
                                .name = "lora" };
static xb_t g_b;
static xb_duty_t g_d;

static void bearer(void)
{
    g_now = 1000; g_air_n = 0; g_epoch = T0_S;
    xb_init(&g_b, &k_ops, "X3SELF");
    xb_set_duty(&g_b, &g_d, lf_ms, NULL, 360000, 6000, 0);
    gate();
    xq_hooks_t h = { .heard_here = h_here, .reached_cheaper = h_cheaper,
                     .epoch_s = h_epoch };
    xq_set_hooks(&g_q, &h);
    xb_set_qos(&g_b, &g_q);
}

static void tick(uint32_t ms)
{
    uint32_t end = g_now + ms;
    while ((int32_t)(end - g_now) > 0) { g_now += 100; xb_tick(&g_b, g_now); }
}

#define SEND(w) xb_send_ex(&g_b, w, (int)strlen(w))
#define OFFER(w) xb_offer(&g_b, w, (int)strlen(w))

static void test_bearer_order(void)
{
    bearer();
    /* Nothing may go: the third-party tier is spent. */
    g_d.spent_ms = 330000; g_d.bucket[g_d.head] = 60000;
    OFFER("t:message f:X1A ts:" T0 " m:third");
    OFFER("t:sos f:X1B ts:" T0 " m:help");
    xb_send_t r = SEND("t:message f:X3SELF ts:" T0 " m:mine");
    CHECK(r == XB_AIRED, "the owner goes past the third-party tier (%d)", r);
    tick(2000);
    CHECK(g_air_n == 2 && strstr(g_aired[1], "t:sos"), "then the sos (%d)", g_air_n);
    tick(5000);
    CHECK(g_air_n == 2, "the third party waits for the reserve (%d)", g_air_n);
}

static void test_bearer_refuses(void)
{
    bearer();
    xb_send_t r = SEND("t:message f:X3SELF scope:local m:room");
    CHECK(r == XB_REFUSED && g_air_n == 0, "scope:local is not for LoRa");
    CHECK(SEND("t:observation f:X3SELF pos:1,2") == XB_AIRED, "first beacon");
    tick(60000);
    CHECK(SEND("t:observation f:X3SELF pos:1,3") == XB_REFUSED, "own beacon a minute later");
    /* One periodic packet per author and type in the queue. */
    OFFER("t:identity f:X1P k:AAAA ts:" T0);
    OFFER("t:identity f:X1P k:AAAA ts:2026-11-25_12:00:05");
    int n = xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL);
    CHECK(n == 1, "one identity queued, not %d", n);
    /* A command for somebody not on this bearer. */
    g_here = false;
    OFFER("t:command f:X1A d:X3FAR ts:" T0 " cmd:history");
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == 1, "not queued");
}

static void test_bearer_expiry_and_eviction(void)
{
    bearer();
    g_d.spent_ms = 330000; g_d.bucket[g_d.head] = 60000;   /* hold third parties */
    OFFER("t:command f:X1A d:X3B ts:" T0 " cmd:history");
    tick(3 * 60000);
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == 0,
          "a transaction waits two minutes, no more");
    CHECK(g_q.count[XQ_TXN].shed[XQ_EXPIRED] == 1, "counted as expired");
    char w[80];
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        snprintf(w, sizeof w, "t:observation f:X3S%02d ts:" T0, i);
        OFFER(w);
    }
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == XB_QUEUE_MAX, "full");
    OFFER("t:message f:X1A ts:" T0 " m:words");
    CHECK(g_q.count[XQ_PRESENCE].shed[XQ_EVICTED] == 1, "presence gave way");
    snprintf(w, sizeof w, "t:observation f:X3S99 ts:" T0);
    OFFER(w);
    CHECK(g_q.count[XQ_PRESENCE].shed[XQ_EVICTED] == 2, "an equal does not push out an equal");
}

static void test_spend_under_qos(void)
{
    bearer();
    g_d.spent_ms = 323000; g_d.bucket[g_d.head] = 1000;
    CHECK(!xb_spend(&g_b, 2000, false), "a relay frame stops at 90%%");
    CHECK(xb_spend(&g_b, 2000, true), "an urgent frame goes");
    char j[512];
    xq_json(&g_q, j, sizeof j);
    CHECK(strstr(j, "\"frames\":1,") != NULL, "a frame, not a class's airing: %s", j);
}

int main(void)
{
    test_fields_and_time();
    test_classes();
    test_scope_binds_everything();
    test_repeat_interval();
    test_destinations();
    test_aired_and_old_news();
    test_reserve_tiers();
    test_pace();
    test_fair_share();
    test_bearer_order();
    test_bearer_refuses();
    test_bearer_expiry_and_eviction();
    test_spend_under_qos();
    printf("xb_qos: %d checks, %d failed; xq_t is %u bytes\n", g_checks, g_fail,
           (unsigned)sizeof(xq_t));
    return g_fail ? 1 : 0;
}
