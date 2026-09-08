/*
 * Host-side test for the battery gauge.
 *
 * The thing this is really protecting against is a four-hour feedback loop.
 * Every question worth asking about a learning gauge -- does the percentage
 * ever go backwards, does a partial discharge teach it anything, does the
 * second cycle read smoother than the first -- takes a full battery and half
 * a day to ask on a board, and can only be asked once per charge. Here a
 * hundred simulated cycles run in under a second.
 */
#include "xprs_power.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

void xpwr_test_reset(void);

static uint32_t g_ms;
static int      g_fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); g_fails++; } \
} while (0)

/* One sample, ten seconds after the last. */
static void feed(int mv)
{
    g_ms += XPWR_SAMPLE_MS;
    assert(xpwr_should_sample(g_ms));
    xpwr_feed(mv, g_ms);
}

static void reset_all(void)
{
    xpwr_test_reset();
    g_ms = 0;
}

/* Put the gauge on a charger, then unplug it with a full cell. */
static void unplug_full(void)
{
    for (int i = 0; i < 8; i++) feed(4450);
    CHECK(xpwr_state() == XPWR_CHARGING, "USB should read as charging");
    feed(4100);
    CHECK(xpwr_state() == XPWR_DISCHARGING, "unplugging should read as discharging");
}

/* Run the cell down from `hi` to `lo` over `steps` samples. */
static void discharge(int hi, int lo, int steps)
{
    for (int i = 1; i <= steps; i++)
        feed(hi + (lo - hi) * i / steps);
}

/* ── 1. A board that has never seen a battery ──────────────────────────── */
static void test_fresh(void)
{
    printf("fresh board\n");
    reset_all();
    xpwr_init();

    CHECK(xpwr_pct() == -1, "no reading yet, pct should be -1, got %d", xpwr_pct());
    CHECK(xpwr_mv() == -1, "no reading yet, mv should be -1");
    CHECK(xpwr_cycles() == 0, "nothing learned yet");
    CHECK(xpwr_confidence() == 0, "confidence should start at zero");

    feed(3820);
    /* The seeded curve puts 3820 mV at the 50%% band. */
    CHECK(xpwr_pct() == 50, "seeded curve should read 50%%, got %d", xpwr_pct());
    CHECK(xpwr_secs_left() == -1,
          "a time remaining must not be offered before it is earned, got %d",
          xpwr_secs_left());
}

/* ── 2. The percentage never climbs on battery ─────────────────────────── */
static void test_ratchet(void)
{
    printf("ratchet\n");
    reset_all();
    xpwr_init();
    unplug_full();

    /* A sagging, rebounding terminal voltage -- what a LoRa TX burst does. */
    int prev = 101;
    int base = 4100;
    for (int i = 0; i < 300; i++) {
        base -= 2;
        int wobble = (i % 7 < 3) ? -60 : +40;   /* far outside the real trend */
        feed(base + wobble);
        int p = xpwr_pct();
        CHECK(p <= prev, "percentage climbed on battery: %d -> %d", prev, p);
        prev = p;
    }
}

/* ── 3. A long enough run teaches it; a short one does not ─────────────── */
static void test_learns_runtime(void)
{
    printf("learning the runtime\n");
    reset_all();
    xpwr_init();

    /* A 20%% dip must teach it nothing: extrapolating a full runtime from a
     * fifth of one is how a gauge ends up confidently wrong. */
    unplug_full();
    discharge(4100, 3990, 60);
    CHECK(xpwr_cycles() == 0, "a 20%% run should not be learned from");
    for (int i = 0; i < 8; i++) feed(4450);       /* back on the charger */
    CHECK(xpwr_cycles() == 0, "still nothing learned, got %u", xpwr_cycles());
    CHECK(xpwr_full_s() == 0, "no runtime should exist yet");

    /* Now a real one: four hours, full to flat. */
    reset_all();
    xpwr_init();
    unplug_full();
    discharge(4100, 3290, 1440);                  /* 1440 * 10 s = 4 h */
    for (int i = 0; i < 8; i++) feed(4450);       /* plugged back in */

    CHECK(xpwr_cycles() == 1, "one full run should be one cycle, got %u",
          xpwr_cycles());
    uint32_t f = xpwr_full_s();
    CHECK(f > 3u * 3600u && f < 5u * 3600u,
          "a four-hour run should learn about four hours, got %lus",
          (unsigned long)f);
    CHECK(xpwr_confidence() == 25, "one cycle should be 25%% confident, got %d",
          xpwr_confidence());
}

/* ── 4. Confidence climbs, and a time remaining appears with it ────────── */
static void test_confidence_ladder(void)
{
    printf("confidence ladder\n");
    reset_all();
    xpwr_init();

    for (int c = 1; c <= 5; c++) {
        unplug_full();
        discharge(4100, 3290, 1440);
        /* Not on the first: the run that teaches it has not closed yet. */
        if (c > 1)
            CHECK(xpwr_secs_left() >= 0,
                  "cycle %d: a time remaining should be offered by now", c);
        for (int i = 0; i < 8; i++) feed(4450);
        int want = (c > 4 ? 4 : c) * 25;
        CHECK(xpwr_confidence() == want,
              "after %d cycles confidence should be %d, got %d",
              c, want, xpwr_confidence());
    }
    CHECK(xpwr_cycles() == 5, "five runs, five cycles, got %u", xpwr_cycles());

    /* Halfway through the sixth, the clock should be most of the answer. */
    unplug_full();
    discharge(4100, 3700, 720);
    int left = xpwr_secs_left();
    CHECK(left > 30 * 60 && left < 3 * 3600,
          "halfway through a four-hour run, expect roughly two hours, got %ds",
          left);
}

/* ── 5. Partial discharges are what a station actually does ────────────── */
static void test_partial_runs(void)
{
    printf("partial runs\n");
    reset_all();
    xpwr_init();

    /* Nobody runs a station flat on purpose. Four 50%% runs, each followed by
     * a recharge, must add up to a learned runtime. */
    for (int c = 0; c < 4; c++) {
        unplug_full();
        discharge(4100, 3810, 720);               /* 100%% -> ~50%%, 2 h */
        for (int i = 0; i < 8; i++) feed(4450);
    }
    CHECK(xpwr_cycles() == 4, "four partial runs should count, got %u",
          xpwr_cycles());
    uint32_t f = xpwr_full_s();
    CHECK(f > 3u * 3600u && f < 5u * 3600u,
          "two hours for half the cell extrapolates to about four, got %lus",
          (unsigned long)f);
}

/* ── 6. The learned state survives a reboot, and garbage does not ──────── */
static void test_persistence(void)
{
    printf("persistence\n");
    reset_all();
    xpwr_init();
    unplug_full();
    discharge(4100, 3290, 1440);
    for (int i = 0; i < 8; i++) feed(4450);
    uint32_t f = xpwr_full_s();
    uint16_t c = xpwr_cycles();
    CHECK(c == 1 && f > 0, "precondition: something was learned");

    /* Reboot: the store survives, the RAM does not. */
    g_ms = 0;
    xpwr_init();
    CHECK(xpwr_full_s() == f, "runtime should survive a reboot: %lu vs %lu",
          (unsigned long)xpwr_full_s(), (unsigned long)f);
    CHECK(xpwr_cycles() == c, "cycles should survive a reboot");

    /* And forgetting really forgets, on this boot and the next. */
    xpwr_forget();
    CHECK(xpwr_cycles() == 0, "forget should clear the cycles");
    xpwr_init();
    CHECK(xpwr_cycles() == 0, "forget should clear the store too, got %u",
          xpwr_cycles());
    CHECK(xpwr_full_s() == 0, "forget should clear the runtime");
}

/* ── 7. The warning fires once per crossing, not once per sample ───────── */
static void test_warning(void)
{
    printf("low battery warning\n");
    reset_all();
    xpwr_init();
    unplug_full();

    char buf[96];
    int lows = 0, crits = 0, prev = 100;
    discharge(4100, 3810, 300);                   /* comfortably above 15%% */
    CHECK(!xpwr_warning(buf, sizeof buf), "no warning at half charge");

    for (int i = 0; i < 600; i++) {
        feed(3810 - (3810 - 3280) * i / 600);
        int p = xpwr_pct();
        if (xpwr_warning(buf, sizeof buf)) {
            if (p <= 5) crits++; else lows++;
            printf("    %d%%: %s\n", p, buf);
        }
        CHECK(p <= prev, "percentage climbed while warning");
        prev = p;
    }
    CHECK(lows == 1, "the low warning should fire exactly once, got %d", lows);
    CHECK(crits == 1, "the critical warning should fire exactly once, got %d",
          crits);
    CHECK(xpwr_critical(), "below 5%% the policy should be told it is critical");
}

/* ── 8. A board with no battery is unchanged ───────────────────────────── */
static void test_no_battery(void)
{
    printf("board with no battery\n");
    reset_all();
    xpwr_init();
    /* xapp calls battery_mv only when the board offers it, but a hook that
     * answers -1 (a T-Deck with nothing on the connector, a Heltec with an
     * empty JST) must also be harmless. */
    for (int i = 0; i < 50; i++) { g_ms += XPWR_SAMPLE_MS; xpwr_feed(-1, g_ms); }
    CHECK(xpwr_pct() == -1, "no cell, no percentage, got %d", xpwr_pct());
    CHECK(xpwr_state() == XPWR_UNKNOWN, "no cell, no state");
    CHECK(!xpwr_critical(), "no cell is not a critical battery");
    char buf[96];
    CHECK(!xpwr_warning(buf, sizeof buf), "no cell, no warning");
}

int main(void)
{
    test_fresh();
    test_ratchet();
    test_learns_runtime();
    test_confidence_ladder();
    test_partial_runs();
    test_persistence();
    test_warning();
    test_no_battery();

    if (g_fails) { printf("\n%d FAILED\n", g_fails); return 1; }
    printf("\nall battery gauge tests passed\n");
    return 0;
}
