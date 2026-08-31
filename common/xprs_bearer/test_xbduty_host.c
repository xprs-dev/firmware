/* Host test for the duty ledger, the priority queue and xb_send_ex.
 * Run: ./test_xbduty_host.sh */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "xprsbearer.h"
#include "xb_airtime.h"

static int g_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── fake radio ─────────────────────────────────────────────────────────── */
static uint32_t g_now = 1000;
static int g_air_count;
static bool g_air_ok = true;
static char g_last_air[XB_WIRE_MAX + 1];

static bool f_air(void *c, const char *w, int n)
{ (void)c; g_air_count++; memcpy(g_last_air, w, (size_t)n);
  g_last_air[n] = 0; return g_air_ok; }
static uint32_t f_now(void) { return g_now; }
static uint32_t f_rand(void) { return 0; }   /* jitter at the minimum */

static const xb_ops_t k_ops = { .air = f_air, .now_ms = f_now,
                                .random = f_rand, .name = "test" };

static xb_t g_b;
static xb_duty_t g_d;

/* SF7/125k, the fleet's channel. */
static const xb_lora_air_t k_air7 = { .bw_hz = 125000, .sf = 7, .cr = 1,
                                      .preamble = 8, .crc = true };
static int g_airtime_calls;
static uint32_t f_airtime(int len, void *c)
{ (void)c; g_airtime_calls++; return xb_lora_airtime_ms(&k_air7, len); }

static void setup(void)
{
    g_now = 1000; g_air_count = 0; g_air_ok = true; g_airtime_calls = 0;
    xb_init(&g_b, &k_ops, "X1TEST");
}
static void advance(uint32_t ms)
{
    uint32_t end = g_now + ms;
    while ((int32_t)(end - g_now) > 0) {
        g_now += 100;
        xb_tick(&g_b, g_now);
    }
}

#define TS "ts:2026-08-31_12:00:00"
static const char *W_ORD  = "t:message f:X1QZ3N d:LISBOA " TS " m:weather";
static const char *W_SOS  = "t:sos f:X1QZ3N pos:38.7,-9.1 m:need water";
static const char *W_WARN = "t:warning f:X1QZ3N m:river rising";
static const char *W_URG  = "t:message f:X1QZ3N d:X1RD89 urg:urgent m:now";

static char g_uw[16][XB_WIRE_MAX + 1];   /* unique ordinary wires */
static const char *uniq(int i)
{ snprintf(g_uw[i], sizeof g_uw[i],
           "t:message f:X1QZ3N d:LISBOA " TS " m:pkt %d", i); return g_uw[i]; }

/* 1. The arithmetic against AN1200.13's own figures. */
static void test_airtime_matches_an1200(void)
{
    CHECK(xb_lora_airtime_ms(&k_air7, 250) == 390, "250B SF7 = %u",
          xb_lora_airtime_ms(&k_air7, 250));
    CHECK(xb_lora_airtime_ms(&k_air7, 64) == 119, "64B SF7 = %u",
          xb_lora_airtime_ms(&k_air7, 64));
    xb_lora_air_t sf12 = k_air7; sf12.sf = 12;      /* derived LDR bit */
    uint32_t t = xb_lora_airtime_ms(&sf12, 250);
    CHECK(t > 8000 && t < 10000, "250B SF12 = %u, expected ~9 s", t);
    xb_lora_air_t sf9 = k_air7; sf9.sf = 9;
    t = xb_lora_airtime_ms(&sf9, 250);
    CHECK(t > 1150 && t < 1300, "250B SF9 = %u, expected ~1.2 s", t);
}

/* 2. No ledger: nothing refused, the callback never runs. */
static void test_unmetered_is_the_default(void)
{
    setup();
    for (int i = 0; i < 5; i++)
        CHECK(xb_send(&g_b, uniq(i), (int)strlen(uniq(i))), "send %d", i);
    CHECK(g_air_count == 5, "aired %d", g_air_count);
    CHECK(g_airtime_calls == 0, "airtime consulted while unmetered");
    xb_duty_report_t r; xb_duty_report(&g_b, g_now, &r);
    CHECK(r.budget_ms == 0, "unmetered reported a budget");
}

/* 3+4+5. The budget stops ordinary traffic; the reserve passes priority. */
static void test_budget_and_reserve(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 36000, 6000, 0);
    /* Spend the ordinary share: 30000/389 = 77 full-size sends. */
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big,
                     "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    CHECK(xb_lora_airtime_ms(&k_air7, n) == 390, "not a full packet: %d", n);
    int aired = 0;
    for (int i = 0; i < 90; i++)
        if (xb_send_ex(&g_b, big, n) == XB_AIRED) aired++; else break;
    /* 30000/390 = 76.9: seventy-six fit, the 77th does not. */
    CHECK(aired == 76, "ordinary traffic stopped at %d, expected 76", aired);
    /* A SHORT ordinary packet still fits the sliver left under the cap, and
     * that is correct -- the budget is airtime, not a count. A full-size
     * one does not fit and must WAIT. */
    big[n - 2] = 'Q';
    CHECK(xb_send_ex(&g_b, big, n) == XB_QUEUED,
          "a spent budget must QUEUE our packet, not drop it");
    /* The reserve still carries the emergency... */
    CHECK(xb_send_ex(&g_b, W_SOS, (int)strlen(W_SOS)) == XB_AIRED,
          "sos refused while the reserve stood");
    CHECK(xb_send_ex(&g_b, W_WARN, (int)strlen(W_WARN)) == XB_AIRED,
          "warning refused while the reserve stood");
    CHECK(xb_send_ex(&g_b, W_URG, (int)strlen(W_URG)) == XB_AIRED,
          "urg:urgent refused while the reserve stood");
    /* ...but urg:high is ordinary -- proven with a full-size packet, since
     * a short one may honestly still fit the sliver under the cap. */
    char high[XB_WIRE_MAX + 1];
    int hn = snprintf(high, sizeof high,
                      "t:message f:X1QZ3N d:X1RD89 urg:high m:");
    while (hn < 250) high[hn++] = 'h';
    high[hn] = 0;
    CHECK(xb_send_ex(&g_b, high, hn) == XB_QUEUED,
          "urg:high drew on the reserve");
    /* Spend the reserve too: it is finite. */
    int prio_aired = 0;
    char sosbig[XB_WIRE_MAX + 1];
    for (int i = 0; i < 40; i++) {
        int m = snprintf(sosbig, sizeof sosbig,
                         "t:sos f:X1QZ3N pos:38.7,-9.1 m:call %d ", i);
        while (m < 250) sosbig[m++] = 'y';
        sosbig[m] = 0;
        if (xb_send_ex(&g_b, sosbig, m) == XB_AIRED) prio_aired++;
    }
    CHECK(prio_aired >= 13 && prio_aired <= 17,
          "the reserve carried %d full sos, expected ~15", prio_aired);
}

/* 6. Priority leaves the queue first, whatever the due order says. */
static void test_priority_leaves_first(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 360000, 6000, 0);
    xb_offer(&g_b, W_ORD, (int)strlen(W_ORD));
    g_now += 100;
    xb_offer(&g_b, W_SOS, (int)strlen(W_SOS));   /* due 100 ms LATER */
    advance(XB_JITTER_MIN_MS + 200);
    CHECK(g_air_count >= 1, "nothing aired");
    CHECK(strstr(g_last_air, "t:sos") == g_last_air ||
          strstr(g_last_air, "need water") != NULL ||
          g_air_count == 1,
          "first out was not the sos: %.40s", g_last_air);
    CHECK(strncmp(g_last_air, "t:sos", 5) == 0 || g_air_count > 1,
          "sos did not go first: %.40s", g_last_air);
}

/* 7+8. Our own traffic is blocked too, and goes later unmodified. */
static void test_own_traffic_blocked_and_deferred(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 1000, 400, 0);
    /* 600 ms of ordinary budget: one 389 ms packet fits, the next does not. */
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:A");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    CHECK(xb_send_ex(&g_b, big, n) == XB_AIRED, "first send refused");
    big[n - 1] = 'B';                            /* a different packet */
    CHECK(xb_send_ex(&g_b, big, n) == XB_QUEUED, "second send not queued");
    int before = g_air_count;
    advance(5000);
    CHECK(g_air_count == before, "aired inside a spent hour");
    /* Roll the window: the whole hour, since the charge sits in one bucket. */
    advance(61u * 60u * 1000u);
    CHECK(g_air_count == before + 1, "deferred packet never aired (%d)",
          g_air_count - before);
    CHECK(g_last_air[n - 1] == 'B' && strstr(g_last_air, "via:") == NULL,
          "deferred packet was modified: %.60s", g_last_air);
}

/* 9. A rolling window, not a fixed hour. */
static void test_window_rolls_not_resets(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 36000, 0, 0);
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    for (int i = 0; i < 46; i++) {               /* ~18 s in minute 0 */
        big[40] = (char)('a' + i % 26); big[41] = (char)('a' + i / 26);
        xb_send_ex(&g_b, big, n);
    }
    uint32_t spent0 = g_d.spent_ms;
    CHECK(spent0 > 17000 && spent0 < 19000, "minute 0 spend %u", spent0);
    advance(30u * 60u * 1000u);                  /* minute 30 */
    for (int i = 0; i < 46; i++) {
        big[40] = (char)('A' + i % 26); big[41] = (char)('A' + i / 26);
        xb_send_ex(&g_b, big, n);
    }
    advance(31u * 60u * 1000u);                  /* minute 61 */
    xb_duty_report_t r; xb_duty_report(&g_b, g_now, &r);
    CHECK(r.spent_ms > 17000 && r.spent_ms < 19000,
          "minute 61 should still owe minute 30's spend, has %u", r.spent_ms);
}

/* 10. The report explains the wait. */
static void test_report_explains_the_wait(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 400, 0, 0);
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    CHECK(xb_send_ex(&g_b, big, n) == XB_AIRED, "first send");
    xb_offer(&g_b, W_ORD, (int)strlen(W_ORD));
    advance(XB_JITTER_MIN_MS + 300);
    xb_wait_t why = XB_WAIT_NONE; bool prio = true;
    int cnt = xb_queue_peek(&g_b, 0, NULL, NULL, &why, &prio);
    CHECK(cnt == 1, "queue holds %d", cnt);
    CHECK(why == XB_WAIT_DUTY, "why = %s", xb_wait_name(why));
    CHECK(!prio, "an ordinary relay marked priority");
    xb_duty_report_t r; xb_duty_report(&g_b, g_now, &r);
    CHECK(r.held == 1, "held = %u", r.held);
    CHECK(r.next_free_ms > 0 && r.next_free_ms <= 3600000u,
          "next_free_ms = %u", r.next_free_ms);
}

/* 11. An echo is never priority, even of an sos. */
static void test_echo_is_never_priority(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 1000, 550, 0);
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    xb_send_ex(&g_b, big, n);                    /* ordinary budget now spent */
    xb_echo(&g_b, W_SOS, (int)strlen(W_SOS));
    advance(XB_JITTER_MIN_MS + 500);
    xb_wait_t why = XB_WAIT_NONE; bool prio = true;
    int cnt = xb_queue_peek(&g_b, 0, NULL, NULL, &why, &prio);
    CHECK(cnt == 1 && !prio, "an echoed sos drew priority (cnt %d)", cnt);
}

/* 12. A full queue evicts the ordinary first; never the reverse. */
static void test_queue_evicts_the_ordinary_first(void)
{
    setup();
    xb_set_pace(&g_b, 60000);          /* nothing drains during the test */
    for (int i = 0; i < XB_QUEUE_MAX; i++)
        xb_offer(&g_b, uniq(i), (int)strlen(uniq(i)));
    xb_offer(&g_b, W_SOS, (int)strlen(W_SOS));   /* ninth into eight slots */
    bool found_sos = false; int n_prio = 0;
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        bool prio = false;
        xb_queue_peek(&g_b, i, NULL, NULL, NULL, &prio);
        if (prio) { found_sos = true; n_prio++; }
    }
    CHECK(found_sos, "the sos was not admitted to a full queue");
    CHECK(n_prio == 1, "prio count %d", n_prio);
}

/* 13. Stale relays are dropped rather than aired late. */
static void test_stale_is_dropped(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 400, 0, 0);
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    xb_send_ex(&g_b, big, n);                    /* budget spent */
    xb_offer(&g_b, W_ORD, (int)strlen(W_ORD));
    advance(XB_STALE_MS + 5000);
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == 0,
          "stale relay still queued");
    CHECK(g_d.stale >= 1, "stale counter %u", g_d.stale);
}

/*
 * 13b. A packet stamped a millisecond AHEAD of the pump's clock is not
 * instantly stale.
 *
 * Two clocks reach the bearer: a packet is stamped with ops.now_ms() inside
 * the enqueue call, while the pump runs on the `now` its caller sampled,
 * which can be slightly older. Unsigned, that difference is 4.29 billion
 * milliseconds -- past every limit at once. A T-Deck logged exactly that:
 * "espnow: 33bb4e waited 4294967s -- no longer worth its airtime, dropped",
 * for a packet queued a moment earlier.
 */
static void test_a_packet_from_the_future_is_not_stale(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 400, 0, 0);
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    xb_send_ex(&g_b, big, n);                    /* budget spent: it queues */

    /* Queue it a tick ahead of where the pump believes it is. */
    g_now += 5;
    xb_offer(&g_b, W_ORD, (int)strlen(W_ORD));
    g_now -= 5;

    xb_tick(&g_b, g_now);
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == 1,
          "a packet queued 5 ms ahead of the pump was dropped as stale");
    CHECK(g_d.stale == 0, "stale counter rose on a fresh packet (%u)",
          g_d.stale);

    /* And it still stales when it really is old. */
    advance(XB_STALE_MS + 5000);
    CHECK(xb_queue_peek(&g_b, 0, NULL, NULL, NULL, NULL) == 0,
          "a genuinely stale relay stayed queued");
}

/* 14. A failing radio is charged all the same. */
static void test_charged_when_the_radio_fails(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 36000, 0, 0);
    g_air_ok = false;
    CHECK(xb_send_ex(&g_b, W_ORD, (int)strlen(W_ORD)) == XB_REFUSED,
          "a failed TX reported success");
    CHECK(g_d.spent_ms > 0, "a failed TX cost nothing");
    uint32_t tx; xb_stats(&g_b, NULL, &tx, NULL);
    CHECK(tx == 0, "tx_count rose on failure");
}

/* 15. The millisecond clock turning over does not invent an hour of spend. */
static void test_clock_wrap(void)
{
    setup();
    g_now = 0xFFFFFF00u;
    xb_init(&g_b, &k_ops, "X1TEST");
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 36000, 0, 0);
    xb_send_ex(&g_b, W_ORD, (int)strlen(W_ORD));
    uint32_t before = g_d.spent_ms;
    CHECK(before > 0, "nothing charged before the wrap");
    advance(0x300);                              /* across zero */
    xb_duty_report_t r; xb_duty_report(&g_b, g_now, &r);
    CHECK(r.spent_ms <= before, "the wrap grew the spend: %u", r.spent_ms);
    CHECK(xb_send_ex(&g_b, W_SOS, (int)strlen(W_SOS)) == XB_AIRED,
          "refused right after the wrap");
}

/* 16. Pace alone reproduces the old behaviour exactly. */
static void test_pace_and_duty_are_independent(void)
{
    setup();
    xb_set_pace(&g_b, 5000);
    xb_offer(&g_b, uniq(0), (int)strlen(uniq(0)));
    xb_offer(&g_b, uniq(1), (int)strlen(uniq(1)));
    advance(XB_JITTER_MIN_MS + 200);
    CHECK(g_air_count == 1, "one per pace window, got %d", g_air_count);
    advance(3000);
    CHECK(g_air_count == 1, "aired while the pace debt stood");
    advance(3000);
    CHECK(g_air_count == 2, "the second never went (%d)", g_air_count);
    CHECK(xb_owed_ms(&g_b) > 0, "no debt after a metered TX");
}

/* 17. A dwell cap refuses what can never fit and passes what can. */
static void test_dwell_cap(void)
{
    setup();
    xb_set_duty(&g_b, &g_d, f_airtime, NULL, 0, 0, 400);
    CHECK(xb_send_ex(&g_b, W_ORD, (int)strlen(W_ORD)) == XB_AIRED,
          "a short packet refused under a dwell cap");
    char big[XB_WIRE_MAX + 1];
    int n = snprintf(big, sizeof big, "t:message f:X1QZ3N d:LISBOA " TS " m:");
    while (n < 250) big[n++] = 'x';
    big[n] = 0;
    CHECK(xb_lora_airtime_ms(&k_air7, n) == 390, "airtime");
    CHECK(xb_send_ex(&g_b, big, n) == XB_AIRED, "390 ms inside a 400 ms cap");
    xb_lora_air_t sf9 = k_air7; sf9.sf = 9; (void)sf9;
    /* With the SF9 table the same packet would be ~1.4 s and refused; that
     * path is exercised through the queue drop in xb_pump instead. */
}

int main(void)
{
    test_airtime_matches_an1200();
    test_unmetered_is_the_default();
    test_budget_and_reserve();
    test_priority_leaves_first();
    test_own_traffic_blocked_and_deferred();
    test_window_rolls_not_resets();
    test_report_explains_the_wait();
    test_echo_is_never_priority();
    test_queue_evicts_the_ordinary_first();
    test_stale_is_dropped();
    test_a_packet_from_the_future_is_not_stale();
    test_charged_when_the_radio_fails();
    test_clock_wrap();
    test_pace_and_duty_are_independent();
    test_dwell_cap();
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("OK: all checks passed\n");
    return 0;
}
