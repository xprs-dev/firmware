/*
 * Host test for the `hears:` ladder.
 *
 * What is checked here is the sender's judgement that XPRS.md 10.6.3 leaves
 * open and never spells out: which neighbours survive a cut list, and what
 * gives way first when the packet runs out of room. Getting that wrong is
 * invisible on a bench with two boards -- every list fits -- and only shows up
 * in the one place it matters, a street with more stations than a packet.
 *
 * The radio is not here. Neighbours are fed in through xst_dev_note and the
 * clock is a variable (the XST_HOST_TEST half of xprs_station.c).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "xprs_station.h"

extern uint32_t xst_test_now_ms;
void xst_test_reset(void);

static int checks, failures;
#define CHECK(cond, fmt, ...) do {                                            \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        printf("  FAIL %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    }                                                                         \
} while (0)

#define SELF "X3WWAJ"

/* A fresh station with nobody heard yet. */
static void reset(void)
{
    xst_test_reset();
    xst_test_now_ms = 1000;
    xst_init(SELF, 0);
}

/* Hear somebody, one millisecond after the last one, so arrival order is
 * definite and the recency tie-break is testable. */
static void hear(const char *call, const char *bearer, int rssi)
{
    xst_test_now_ms += 1;
    xst_dev_note(call, bearer, rssi);
}

static int render(const char *bearer, int budget, char *calls, int *total,
                  char *q)
{
    char qbuf[XST_SEEN_MAX + 1];
    if (!q) q = qbuf;
    return xst_hears_render(bearer, 600, budget, calls, 208, total,
                            q, XST_SEEN_MAX + 1);
}

/* ── The bucket ─────────────────────────────────────────────────────────── */

static void test_bucket(void)
{
    /* Nine is loud, zero is barely there, and the ends are clamped rather
     * than allowed to run off either side. */
    CHECK(xst_signal_bucket(-20, 0xff) == 9, "clamped high");
    CHECK(xst_signal_bucket(-30, 0xff) == 9, "-30 dBm is the top");
    CHECK(xst_signal_bucket(-100, 0xff) == 0, "-100 dBm is the bottom");
    CHECK(xst_signal_bucket(-140, 0xff) == 0, "clamped low");
    CHECK(xst_signal_bucket(-65, 0xff) == 5, "mid scale");

    /* Seven dB a step, so each boundary lands where it is claimed to. */
    CHECK(xst_signal_bucket(-93, 0xff) == 1, "-93 is one step up");
    CHECK(xst_signal_bucket(-94, 0xff) == 0, "-94 is still the bottom");

    /* The hysteresis, which is the whole reason the bucket is stored. A
     * reading that has drifted one dB past a boundary keeps the old answer;
     * one that is properly clear of it does not. */
    CHECK(xst_signal_bucket(-57, 5) == 5, "one dB over the edge holds");
    CHECK(xst_signal_bucket(-54, 5) == 6, "three dB clear moves up");
    CHECK(xst_signal_bucket(-66, 5) == 5, "one dB under the edge holds");
    CHECK(xst_signal_bucket(-69, 5) == 4, "three dB clear moves down");
    /* Stickiness is half a step, not a licence to never move. */
    CHECK(xst_signal_bucket(-40, 5) == 8, "a real change is not resisted");
    CHECK(xst_signal_bucket(-90, 5) == 1, "in either direction");
}

/* ── Tier 1: everybody, with their digits ───────────────────────────────── */

static void test_tier1(void)
{
    reset();
    hear("X3LTSH", "espnow", -35);
    hear("X3R8XX", "espnow", -60);
    hear("X1GUD9", "espnow", -80);

    char calls[208], q[XST_SEEN_MAX + 1];
    int total = 0;
    int n = render("espnow", 200, calls, &total, q);

    CHECK(total == 3, "peers counts three, got %d", total);
    CHECK(strcmp(calls, "X3LTSH,X3R8XX,X1GUD9") == 0, "list is \"%s\"", calls);
    CHECK(strcmp(q, "952") == 0, "digits are \"%s\"", q);
    CHECK(n == (int)strlen(calls), "returns what it wrote");
    CHECK((int)strlen(q) == 3, "one digit a callsign");
}

/* ── The ranking 10.6.3 leaves to the sender ────────────────────────────── */

static void test_ranking(void)
{
    /* Section 2: X3 is a station or relay, X1 is a person. A reader picking a
     * carrier wants the relay, so a cut list must keep it -- even when the
     * person is louder and arrived later. */
    reset();
    hear("X1GUD9", "espnow", -35);      /* loud, fresh, and a person */
    hear("X3LTSH", "espnow", -85);      /* faint, older, and a relay */

    char calls[208];
    int total = 0;
    render("espnow", 200, calls, &total, NULL);
    CHECK(strncmp(calls, "X3LTSH", 6) == 0, "relay first, got \"%s\"", calls);

    /* Within a class it is the loudest, and then the freshest. */
    reset();
    hear("X3AAAA", "espnow", -80);
    hear("X3BBBB", "espnow", -40);
    render("espnow", 200, calls, &total, NULL);
    CHECK(strcmp(calls, "X3BBBB,X3AAAA") == 0, "loudest first, \"%s\"", calls);

    reset();
    hear("X3AAAA", "espnow", -50);
    hear("X3BBBB", "espnow", -50);      /* same bucket, heard later */
    render("espnow", 200, calls, &total, NULL);
    CHECK(strcmp(calls, "X3BBBB,X3AAAA") == 0, "freshest breaks it, \"%s\"",
          calls);

    /* X4 is equipment under a controller and outranks a person; anything
     * that is not a callsign this format knows sorts last. */
    reset();
    hear("X1AAAA", "espnow", -40);
    hear("X4BBBB", "espnow", -40);
    hear("CT1ABC", "espnow", -40);
    hear("X3CCCC", "espnow", -40);
    render("espnow", 200, calls, &total, NULL);
    CHECK(strcmp(calls, "X3CCCC,X4BBBB,X1AAAA,CT1ABC") == 0,
          "class order is \"%s\"", calls);
}

/* ── Tier 2: signal gives way before names do ───────────────────────────── */

static void test_tier2(void)
{
    reset();
    hear("X3AAAA", "espnow", -40);
    hear("X3BBBB", "espnow", -50);
    hear("X3CCCC", "espnow", -60);

    /* " hears:" is 7, the list is 20, " zhq:" is 5 and the digits are 3:
     * 35 bytes for everything, 27 for the names alone. Between the two, the
     * digits are what must go -- and the list must stay whole. */
    char calls[208], q[XST_SEEN_MAX + 1];
    int total = 0;
    render("espnow", 30, calls, &total, q);
    CHECK(strcmp(calls, "X3AAAA,X3BBBB,X3CCCC") == 0, "whole list, \"%s\"",
          calls);
    CHECK(q[0] == 0, "digits dropped, got \"%s\"", q);
    CHECK(total == 3, "peers still three");

    /* One byte more and both fit again. */
    render("espnow", 35, calls, &total, q);
    CHECK(strcmp(q, "875") == 0, "digits return at 35, got \"%s\"", q);
}

/* ── Tier 3: the top-ranked that fit, and an honest peers: ──────────────── */

static void test_tier3(void)
{
    reset();
    hear("X1AAAA", "espnow", -90);
    hear("X1BBBB", "espnow", -90);
    hear("X3CCCC", "espnow", -90);      /* the relay, heard last */

    /* 7 for " hears:" and 13 for two six-character names with a comma. */
    char calls[208], q[XST_SEEN_MAX + 1];
    int total = 0;
    render("espnow", 20, calls, &total, q);
    CHECK(strcmp(calls, "X3CCCC,X1BBBB") == 0, "kept the useful half, \"%s\"",
          calls);
    CHECK(q[0] == 0, "no digits in a cut list");
    CHECK(total == 3, "peers is the true count, got %d", total);

    /* 10.6.4: a cut list is always visibly cut. */
    CHECK(total > 2, "peers exceeds the list, which is what says it was cut");

    /* Too tight for even one name is not a mangled name. */
    render("espnow", 10, calls, &total, q);
    CHECK(calls[0] == 0, "nothing rather than a fragment, \"%s\"", calls);
    CHECK(total == 3, "and peers still tells the truth");
}

/* ── Per-bearer truth, and the bearer with no signal ────────────────────── */

static void test_bearers(void)
{
    reset();
    hear("X3AAAA", "espnow", -40);
    hear("X3BBBB", "lan", 0);

    char calls[208], q[XST_SEEN_MAX + 1];
    int total = 0;

    render("espnow", 200, calls, &total, q);
    CHECK(strcmp(calls, "X3AAAA") == 0, "espnow lists its own, \"%s\"", calls);
    CHECK(total == 1, "and counts only its own, got %d", total);
    CHECK(strcmp(q, "8") == 0, "with its digit, got \"%s\"", q);

    /* The LAN has no RSSI, so it says nothing about signal rather than
     * inventing a zero that would read as "barely there". */
    render("lan", 200, calls, &total, q);
    CHECK(strcmp(calls, "X3BBBB") == 0, "lan lists its own, \"%s\"", calls);
    CHECK(q[0] == 0, "no digits without signal, got \"%s\"", q);

    /* One neighbour without signal is enough to withhold the whole string:
     * a digit per callsign cannot have a hole in it. */
    reset();
    hear("X3AAAA", "espnow", -40);
    hear("X3BBBB", "espnow", 0);
    render("espnow", 200, calls, &total, q);
    CHECK(total == 2, "both are neighbours");
    CHECK(q[0] == 0, "and neither gets a digit, got \"%s\"", q);
}

/* ── Only what was heard directly ───────────────────────────────────────── */

static void test_direct_only(void)
{
    /* A callsign somebody's relay carried to us is not a neighbour. Nothing
     * here can produce one -- xst_dev_note records direct sightings -- so the
     * check is that an empty answer is empty, not a stale buffer. */
    reset();
    char calls[208], q[XST_SEEN_MAX + 1];
    int total = -1;
    int n = render("espnow", 200, calls, &total, q);
    CHECK(n == 0, "nobody heard yet");
    CHECK(total == 0, "and peers says so, got %d", total);
    CHECK(calls[0] == 0 && q[0] == 0, "both buffers cleared");
}

/* ── A full table, which is the case the hardware cannot make ───────────── */

static void test_full_table(void)
{
    reset();
    /* Sixteen relays, all the same strength, is the worst the store can
     * hold: XST_SEEN_MAX rows of six characters each. */
    for (int i = 0; i < XST_SEEN_MAX; i++) {
        char c[8];
        snprintf(c, sizeof c, "X3%c%c%c%c", 'A' + i, 'A' + i, 'A' + i, 'A' + i);
        hear(c, "espnow", -50);
    }

    char calls[208], q[XST_SEEN_MAX + 1];
    int total = 0;

    /* 141 is what a signed observation from a six-character callsign really
     * has for its suffix: 250 less a 44-byte head and 65 for the signature.
     * Sixteen names is 111, and " hears:" + " zhq:" + sixteen digits brings
     * it to 139 -- so the whole table fits, with its signal, and two bytes to
     * spare. Seventeen would not, which is why the ladder exists. */
    render("espnow", 141, calls, &total, q);
    CHECK(total == XST_SEEN_MAX, "sixteen neighbours, got %d", total);
    CHECK((int)strlen(q) == XST_SEEN_MAX, "sixteen digits, got %zu",
          strlen(q));
    CHECK(7 + (int)strlen(calls) + 5 + (int)strlen(q) == 139,
          "and the whole suffix is 139 bytes");
}

int main(void)
{
    printf("xprs_station host tests (XPRS.md 10.6.3, 10.6.4)\n");
    test_bucket();
    test_tier1();
    test_ranking();
    test_tier2();
    test_tier3();
    test_bearers();
    test_direct_only();
    test_full_table();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
