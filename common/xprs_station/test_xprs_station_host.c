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
#include "xprs.h"

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

/* Note a 1:1 into the chat ring, as the ingest path would. */
static void chat_msg(const char *from, const char *to, const char *text)
{
    xst_test_now_ms += 1;
    char wire[160];
    snprintf(wire, sizeof wire,
             "t:message f:%s d:%s ts:2026-09-05_08:00:00 m:%s", from, to, text);
    xprs_t p;
    if (xprs_parse(wire, (int)strlen(wire), &p)) xst_chat_note(&p);
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

/* The clock ladder (XPRS.md 11.10): who may set the time, and never back. */
static void test_clock(void)
{
    extern uint32_t xst_test_clock;
    const uint32_t t0 = 1789819440u;                /* 2026-09-19_12:04:00 */
    CHECK(xst_clock_src() == XST_CLK_NONE, "no clock at start");
    CHECK(!xst_clock_set(XST_CLK_OWNER, 1000u, "X1A"), "1970 is refused");
    CHECK(!xst_clock_set(XST_CLK_OWNER, 2300000000u, "X1A"), "2042 is refused");
    CHECK(xst_test_clock == 0, "nothing was set");
    CHECK(xst_clock_set(XST_CLK_HINT, t0, "neighbours"), "a hint fills a void");
    CHECK(xst_test_clock == t0 && xst_clock_src() == XST_CLK_HINT, "the hint set it");
    CHECK(!xst_clock_set(XST_CLK_HINT, t0 - 600u, "others"), "a hint never turns it back");
    CHECK(xst_clock_set(XST_CLK_HINT, t0 + 600u, "others"), "a later hint moves it on");
    CHECK(!xst_clock_set(XST_CLK_OWNER, t0 - 3600u, "X1A"),
          "not even the owner turns a valid clock back");
    CHECK(xst_clock_set(XST_CLK_OWNER, t0 + 1200u, "X1A"), "the owner moves it on, and owns it");
    CHECK(xst_clock_src() == XST_CLK_OWNER && xst_test_clock == t0 + 1200u, "owner now");
    CHECK(!xst_clock_set(XST_CLK_HINT, t0 + 7200u, "others"), "a hint never moves the owner's");
    CHECK(!xst_clock_set(XST_CLK_OWNER, t0 - 7200u, "X1A"), "the owner cannot turn it back");
    CHECK(xst_clock_set(XST_CLK_OWNER, t0 + 2400u, "X1A"), "the owner moves it forward");
    xst_clock_from_ntp();
    CHECK(xst_clock_src() == XST_CLK_NTP && strcmp(xst_clock_src_name(), "ntp") == 0, "ntp");
    CHECK(!xst_clock_set(XST_CLK_OWNER, t0 + 99999u, "X1A"), "nobody moves an NTP clock");
    CHECK(!xst_clock_set(XST_CLK_HINT, t0 + 99999u, "others"), "nor a hint");
}

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

    /* A full table of six-character callsigns costs 6 each plus a comma
     * between: XST_SEEN_MAX * 7 - 1 bytes. With " hears:" (7), " zhq:" (5)
     * and one digit per name, the whole thing needs
     * 7 + (7n - 1) + 5 + n bytes, which at 24 rows is 203.
     *
     * This used to be written out as "sixteen names is 111 ... 139 bytes",
     * and then XST_SEEN_MAX became 24 (d015b9c, one row per callsign AND
     * bearer). The ladder did exactly what it exists for and the arithmetic
     * in this test was the only thing still living in a sixteen-row world,
     * so it is computed here instead of quoted. */
    const int names = XST_SEEN_MAX * 7 - 1;
    const int whole = 7 + names + 5 + XST_SEEN_MAX;

    render("espnow", whole, calls, &total, q);
    CHECK(total == XST_SEEN_MAX, "every neighbour, got %d", total);
    CHECK((int)strlen(calls) == names, "every name, got %zu", strlen(calls));
    CHECK((int)strlen(q) == XST_SEEN_MAX, "a digit each, got %zu", strlen(q));
    CHECK(7 + (int)strlen(calls) + 5 + (int)strlen(q) == whole,
          "and the whole suffix is exactly the budget");

    /* 141 is what a signed observation from a six-character callsign really
     * has for its suffix: 250 less a 44-byte head and 65 for the signature --
     * less than the whole table now costs. Signal is what gives way first,
     * and only then callsigns: the reply still fits, still counts every
     * neighbour in `zhc:`, and carries as many names as the budget allows. */
    memset(q, 0, sizeof q);
    render("espnow", 141, calls, &total, q);
    CHECK(total == XST_SEEN_MAX,
          "the true count survives the cut (10.6.4), got %d", total);
    CHECK(q[0] == 0, "the digits gave way, got '%s'", q);
    CHECK(7 + (int)strlen(calls) <= 141, "and what is left fits the budget");
    CHECK((int)strlen(calls) > names / 2, "with most of the names kept");
}

/* ── One station, one row ───────────────────────────────────────────────── */

static void test_one_row_per_station(void)
{
    /* The table keys on (callsign, bearer) for the beacon's sake, and the
     * radar read it raw: three stations, nine blips (bench 2026-09-04). */
    reset();
    hear("X1VCVM", "ble", -55);
    hear("X1VCVM", "lan", 0);
    hear("X1VCVM", "lora", -26);
    uint32_t lora_at = xst_test_now_ms;

    xst_dev_t d[XST_SEEN_MAX];
    int n = xst_devices(d, XST_SEEN_MAX, 600);
    CHECK(n == 1, "one station, one row, got %d", n);
    CHECK(strcmp(d[0].bearer, "lora") == 0,
          "wearing its latest contact, got %s", d[0].bearer);
    CHECK(d[0].rssi == -26, "with that contact's signal, got %d", d[0].rssi);
    CHECK(d[0].last_ms == lora_at, "and its age");
    CHECK(xst_devices_links(d, XST_SEEN_MAX, 600) == 3,
          "the beacon still sees three links");
    CHECK(xst_devices_in_range(600) == 1,
          "in reach counts stations, got %d", xst_devices_in_range(600));

    /* The LAN heard it last and the LAN has no signal: the distance is
     * borrowed from the strongest radio reading inside the window. */
    reset();
    hear("X1VCVM", "lora", -70);
    hear("X1VCVM", "ble", -55);
    hear("X1VCVM", "lan", 0);
    uint32_t lan_at = xst_test_now_ms;
    n = xst_devices(d, XST_SEEN_MAX, 600);
    CHECK(n == 1, "still one row, got %d", n);
    CHECK(strcmp(d[0].bearer, "lan") == 0, "latest contact is the LAN");
    CHECK(d[0].rssi == -55, "distance borrowed from the loudest radio, got %d",
          d[0].rssi);
    CHECK(d[0].last_ms == lan_at, "age is the LAN's");

    /* A reading that fell out of the window is not borrowed from. */
    xst_test_now_ms += 700 * 1000;
    hear("X1VCVM", "lan", 0);
    n = xst_devices(d, XST_SEEN_MAX, 600);
    CHECK(n == 1 && d[0].rssi == 0, "no fresh radio reading, no distance");

    /* Two stations stay two. */
    hear("X3S7S8", "ble", -65);
    n = xst_devices(d, XST_SEEN_MAX, 600);
    CHECK(n == 2, "two stations, got %d", n);
    CHECK(xst_devices_in_range(600) == 2, "and counted as two");
}

static void test_chat_peers(void)
{
    reset();
    /* Heard but never messaged: NOT a peer. */
    hear("X3AAAA", "espnow", -50);
    hear("X5GRP", "espnow", -50);          /* a group, never a 1:1 */
    /* Two exchanges, both directions, newest last. */
    chat_msg("X1ZEBRA", SELF, "hi from zebra");   /* incoming */
    chat_msg(SELF, "X1ALPHA", "hi to alpha");     /* outgoing */
    chat_msg("X1ALPHA-2", SELF, "same person, suffix");  /* base collapses */

    char peers[8][XPRS_CALL_LEN];
    int n = xst_chat_peers(SELF, peers, 8);
    CHECK(n == 2, "two exchanged peers, got %d", n);
    CHECK(strcmp(peers[0], "X1ALPHA") == 0, "alphabetical first, got %s", peers[0]);
    CHECK(strcmp(peers[1], "X1ZEBRA") == 0, "alphabetical second, got %s", peers[1]);
    /* A device merely heard is absent. */
    for (int i = 0; i < n; i++)
        CHECK(strcmp(peers[i], "X3AAAA") != 0, "a heard non-peer leaked in");

    /* The cap is honoured. */
    chat_msg(SELF, "X1BRAVO", "x");
    chat_msg(SELF, "X1CHARLIE", "x");
    n = xst_chat_peers(SELF, peers, 2);
    CHECK(n == 2, "cap respected, got %d", n);
}

/* A local-room message with its own ts:, as heard. */
static void chat_said(const char *from, const char *ts, const char *text)
{
    xst_test_now_ms += 1;
    char wire[160];
    snprintf(wire, sizeof wire,
             "t:message f:%s ts:%s scope:local m:%s", from, ts, text);
    xprs_t p;
    if (xprs_parse(wire, (int)strlen(wire), &p)) xst_chat_note(&p);
}

/* The mesh replays: a message from an hour ago arrives again. It must not
 * come back as the newest saying, nor push a newer one out of a full ring
 * (2026-10-07: screens showed replays and lost the C61's live messages). */
static void test_chat_replay(void)
{
    char ts[24], text[16];
    for (int i = 0; i < XST_CHAT_MAX; i++) {
        snprintf(ts, sizeof ts, "2026-09-05_10:%02d:00", i);
        snprintf(text, sizeof text, "fill %d", i);
        chat_said("X1FILL", ts, text);
    }
    chat_said("X1OLD", "2026-09-05_09:00:00", "an hour-old replay");
    chat_said("X1NEW", "2026-09-05_11:00:00", "said just now");
    xst_chat_t rows[XST_CHAT_MAX];
    int n = xst_chat(rows, XST_CHAT_MAX);
    CHECK(n > 0 && strcmp(rows[0].text, "said just now") == 0,
          "the newest saying is the newest row, whatever arrived after it");
    bool replay = false;
    for (int i = 0; i < n; i++)
        if (strcmp(rows[i].from, "X1OLD") == 0) replay = true;
    CHECK(!replay, "a replay older than a full ring stays out of it");
    chat_said("X1LATE", "2026-09-05_10:59:30", "arrived after, said before");
    n = xst_chat(rows, XST_CHAT_MAX);
    CHECK(strcmp(rows[0].from, "X1NEW") == 0 &&
          strcmp(rows[1].from, "X1LATE") == 0,
          "ordered by when it was said, not by when it arrived");
}

static void test_heard(void)
{
    reset();
    hear("X1NEAR", "ble", -55);
    hear("X1NEAR-3", "lan", 0);            /* same base, other bearer */
    CHECK(xst_heard("X1NEAR", 300), "a fresh station is heard");
    CHECK(xst_heard("x1near-9", 300), "base + case-insensitive");
    CHECK(!xst_heard("X1GONE", 300), "an unheard station is not");
    /* Age it out. */
    xst_test_now_ms += 301u * 1000u;
    CHECK(!xst_heard("X1NEAR", 300), "stale is not heard");
}

/* XPRS.md 9.11: the regional room, and a room's traffic heard out of range. */
static void test_scope_rooms(void)
{
    reset();
    struct { const char *w; const char *bearer; int kind; } c[] = {
        { "t:message f:X1REG1 ts:2026-09-05_08:00:00 scope:regional m:valley", "lora", 4 },
        { "t:message f:X1LOC1 ts:2026-09-05_08:00:01 scope:local m:room",      "ble",  1 },
        { "t:message f:X1GLO1 ts:2026-09-05_08:00:02 m:world",                 "rns",  0 },
        { "t:message f:X1FAR1 ts:2026-09-05_08:00:03 scope:local m:afar",      "rns", -1 },
        { "t:message f:X1FAR2 ts:2026-09-05_08:00:04 scope:regional m:afar",   "rns", -1 },
        { "t:message f:X1LOR1 ts:2026-09-05_08:00:05 scope:local m:lora",      "lora", -1 },
    };
    xst_set_lora_local(false);
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        xst_test_now_ms += 1;
        xprs_t p;
        CHECK(xprs_parse(c[i].w, (int)strlen(c[i].w), &p), "parse %zu", i);
        xst_ingest_parsed(&p, c[i].bearer, -60);
    }
    xst_chat_t rows[16];
    int n = xst_chat(rows, 16);
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char call[8];
        snprintf(call, sizeof call, "%.6s", c[i].w + 12);
        int kind = -1;
        for (int j = 0; j < n; j++) if (!strcmp(rows[j].from, call)) kind = rows[j].kind;
        CHECK(kind == c[i].kind, "%s: kind %d, want %d", call, kind, c[i].kind);
    }
    /* The operator puts LoRa in the local class: the campsite's room. */
    xst_set_lora_local(true);
    xprs_t p;
    const char *w = "t:message f:X1LOR2 ts:2026-09-05_08:00:06 scope:local m:camp";
    xprs_parse(w, (int)strlen(w), &p);
    xst_ingest_parsed(&p, "lora", -60);
    n = xst_chat(rows, 16);
    bool seen = false;
    for (int j = 0; j < n; j++) if (!strcmp(rows[j].from, "X1LOR2")) seen = rows[j].kind == 1;
    CHECK(seen, "a local LoRa wasn't a local room");
    xst_set_lora_local(false);
}

/* ── A relayed station's distance (xst_dev_path) ───────────────────────── */

/* Ingest one wire as the station would, on [bearer] at [rssi]. */
static void ingest_via(const char *wire, const char *bearer, int rssi,
                       int more, const char *relay)
{
    xst_test_now_ms += 1;
    xprs_t p;
    if (xprs_parse(wire, (int)strlen(wire), &p))
        xst_ingest_parsed_ex(&p, bearer, rssi, more, relay);
}

static void ingest(const char *wire, const char *bearer, int rssi, int more)
{
    ingest_via(wire, bearer, rssi, more, "");
}

static int row_of(const char *call, xst_dev_t *out)
{
    xst_dev_t rows[XST_SEEN_MAX];
    int n = xst_devices(rows, XST_SEEN_MAX, 300);
    for (int i = 0; i < n; i++)
        if (strcmp(rows[i].call, call) == 0) { *out = rows[i]; return 1; }
    return 0;
}

static void test_path(void)
{
    reset();
    xst_dev_t d;
    xst_path_t pt;
    const int m50 = (int)(xst_est_distance_m(-50) + 0.5f);
    const int m62 = (int)(xst_est_distance_m(-62) + 0.5f);   /* digit 5 */
    const int m70 = (int)(xst_est_distance_m(-70) + 0.5f);

    /* A neighbour: the whole distance is the near leg. */
    hear("X3SEP2", "espnow", -50);
    CHECK(row_of("X3SEP2", &d) && d.hops == 0 && d.via[0] == 0, "neighbour");
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == m50 && pt.near_m == m50 && pt.far_m == 0 &&
          pt.relay[0] == 0, "neighbour: total %d near %d far %d",
          pt.total_m, pt.near_m, pt.far_m);

    /* A Meshtastic node the neighbour bridged: the packet's signal is the
     * neighbour's. Until the neighbour says how far the node is, the total
     * is unknown and the near leg is a floor. */
    ingest("t:message f:MT1234 via:X3SEP2 m:hi", "espnow", -50, 0);
    CHECK(row_of("MT1234", &d) && d.hops == 1 && !strcmp(d.via, "X3SEP2"),
          "relayed row: hops %d via '%s'", d.hops, d.via);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == -1 && pt.near_m == m50 && pt.far_m == -1 &&
          pt.floor_m == m50 && !strcmp(pt.relay, "X3SEP2"),
          "unsaid: total %d near %d far %d floor %d '%s'",
          pt.total_m, pt.near_m, pt.far_m, pt.floor_m, pt.relay);

    /* The neighbour's observation says it hears the node at digit 5. */
    xst_far_note("X3SEP2", "MT1234", 5);
    CHECK(xst_far_rssi("X3SEP2", "MT1234", 900) == -62, "digit 5 is -62 dBm");
    CHECK(xst_far_rssi("X3SEP2", "MT9999", 900) == 0, "never said");
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == m50 + m62 && pt.far_m == m62 && pt.floor_m == pt.total_m,
          "summed: total %d = %d + %d", pt.total_m, m50, m62);

    /* The near leg prefers the relay's own fresh direct row over the one
     * packet: the relay now heard louder. */
    hear("X3SEP2", "espnow", -40);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.near_m == (int)(xst_est_distance_m(-40) + 0.5f),
          "near leg from the relay's row: %d", pt.near_m);

    /* The saying ages out. */
    xst_test_now_ms += 901 * 1000;
    CHECK(xst_far_rssi("X3SEP2", "MT1234", 900) == 0, "stale saying");

    /* Two relays: node -> X1GATE -> X3SEP2 -> us. Both legs must be said. */
    reset();
    hear("X3SEP2", "espnow", -50);
    ingest("t:message f:MT1234 via:X1GATE,X3SEP2 m:hi", "espnow", -50, 0);
    CHECK(row_of("MT1234", &d) && d.hops == 2 &&
          !strcmp(d.via, "X1GATE,X3SEP2"), "two hops, via '%s'", d.via);
    xst_far_note("X3SEP2", "X1GATE", 5);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == -1 && !strcmp(pt.relay, "X3SEP2") &&
          pt.floor_m == m50 + m62, "gate unsaid: floor %d", pt.floor_m);
    xst_far_note("X1GATE", "MT1234", 5);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == m50 + 2 * m62, "three legs: %d", pt.total_m);

    /* A chain longer than the tail kept: the farthest legs are unknown. */
    ingest("t:message f:X1FAR via:X1AAAAAA,X1BBBBBB,X1CCCCCC m:hi", "espnow",
           -50, 0);
    CHECK(row_of("X1FAR", &d) && d.hops == 3 &&
          !strcmp(d.via, "X1BBBBBB,X1CCCCCC"), "tail kept: '%s'", d.via);
    xst_far_note("X1CCCCCC", "X1BBBBBB", 9);
    xst_far_note("X1BBBBBB", "X1FAR", 9);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == -1 && pt.far_m == -1, "a cut chain is unknown");

    /* On the gateway itself the translation names the gateway: the node is
     * a neighbour there, at the frame's own signal. */
    reset();
    ingest("t:message f:MT1234 via:" SELF " m:hi", "lora", -70, 0);
    CHECK(row_of("MT1234", &d) && d.hops == 0 && d.via[0] == 0,
          "own callsign is not a hop: hops %d via '%s'", d.hops, d.via);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == m70, "gateway sees the node at %d", pt.total_m);

    /* ...unless Meshtastic's own repeaters carried the frame: then the
     * signal is the last repeater's, the hops are counted, and what the
     * radio measured is the floor -- the distance to that repeater. */
    ingest("t:message f:MT5678 via:" SELF " m:hi", "lora", -70, 2);
    CHECK(row_of("MT5678", &d) && d.hops == 2 && d.via[0] == 0 && d.rssi == -70,
          "meshtastic hops: %d rssi %d", d.hops, d.rssi);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == -1 && pt.near_m == m70 && pt.floor_m == m70 &&
          pt.relay[0] == 0, "heard through an unnamed repeater %d m away",
          pt.floor_m);
    /* The bridge named the repeater: it is the row's relay, and its own
     * direct sighting gives the near leg. */
    xst_dev_note_hops("MTREPEAT", "lora", -70, 0, "");
    ingest_via("t:message f:MT5678 via:" SELF " m:hi", "lora", -70, 2, "MTREPEAT");
    CHECK(row_of("MT5678", &d) && d.hops == 2 && !strcmp(d.via, "MTREPEAT"),
          "named repeater: via '%s'", d.via);
    xst_dev_path(&d, 300, &pt);
    CHECK(pt.total_m == -1 && pt.floor_m == m70 && !strcmp(pt.relay, "MTREPEAT"),
          "floor to the named repeater: %d via %s", pt.floor_m, pt.relay);
    /* A bridge sighting with hops keeps its signal too. */
    xst_dev_note_hops("MT9999", "lora", -70, 3, NULL);
    CHECK(row_of("MT9999", &d) && d.hops == 3 && d.rssi == -70 && d.via[0] == 0,
          "sighting behind repeaters keeps the reading");

    /* One row per station: the shortest path wins, and a relayed copy's
     * signal never stands in for a direct one. */
    reset();
    ingest("t:message f:X1DUAL m:a", "lora", -80, 0);
    ingest("t:message f:X1DUAL via:X3SEP2 m:a", "espnow", -40, 0);
    CHECK(row_of("X1DUAL", &d) && d.hops == 0 && d.rssi == -80 &&
          !strcmp(d.bearer, "espnow"), "direct path kept: hops %d rssi %d on %s",
          d.hops, d.rssi, d.bearer);
    ingest("t:message f:X1LAN m:a", "lan", 0, 0);
    ingest("t:message f:X1LAN via:X3SEP2 m:a", "espnow", -40, 0);
    CHECK(row_of("X1LAN", &d) && d.hops == 0 && d.rssi == 0,
          "a LAN neighbour has no distance, not the relay's");
}

int main(void)
{
    printf("xprs_station host tests (XPRS.md 10.6.3, 10.6.4)\n");
    test_bucket();
    test_clock();
    test_tier1();
    test_ranking();
    test_tier2();
    test_tier3();
    test_bearers();
    test_direct_only();
    test_full_table();
    test_one_row_per_station();
    test_chat_peers();
    test_chat_replay();
    test_scope_rooms();
    test_heard();
    test_path();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
