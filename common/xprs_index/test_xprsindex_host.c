/*
 * Host test for the XPRS index. Runs on a temp directory instead of a card, so
 * the record layout, the two derived indexes, the eviction of what must not be
 * served and the recovery path are all testable without hardware.
 *
 * Build + run:  ./test_xprsindex_host.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xprsindex.h"

static int g_fail = 0;
/* The store converts ts: to epoch internally; the test needs the same number. */
static uint32_t xi_expect_ts(const char *str)
{
    int Y,M,D,h,m,sec;
    if (sscanf(str, "%4d-%2d-%2d_%2d:%2d:%2d", &Y,&M,&D,&h,&m,&sec) != 6) return 0;
    static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    long days = (long)(Y - 1970) * 365 + ((Y - 1969) / 4) + cum[M - 1] + (D - 1);
    if (M > 2 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) days++;
    return (uint32_t)(days * 86400L + h * 3600 + m * 60 + sec);
}

static int g_checks = 0;

#define CHECK(cond, ...) do {                                              \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
        g_fail++;                                                          \
        printf("  FAIL %s:%d  ", __func__, __LINE__);                      \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                      \
    }                                                                      \
} while (0)

/* 2026-08-13_09:00:00 and a year earlier, as the epochs the codec derives. */
#define TS_2026 "2026-08-13_09:00:00"
#define TS_2025 "2025-08-13_09:00:00"

typedef struct { int n; xprsidx_rec_t last; xprsidx_rec_t first; } collect_t;

static bool collect(const xprsidx_rec_t *r, void *ctx)
{
    collect_t *c = ctx;
    if (c->n == 0) c->first = *r;
    c->last = *r;
    c->n++;
    return true;
}

static void rm_rf(const char *dir)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* first run: nothing to remove */ }
}

/* ── the two questions the user asked for ───────────────────────────────── */

static void test_recent_of_a_type(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");

    /* Noise, then warnings, then more noise: the warnings are NOT at the end,
     * so a naive "read the last N records" would get this wrong. */
    char w[280];
    for (int i = 0; i < 40; i++) {
        snprintf(w, sizeof w, "t:observation f:X3WX%02d link:ble peers:2 ts:%s", i, TS_2026);
        xprsindex_add(st, w, (int)strlen(w), -60, false, 0);
    }
    for (int i = 0; i < 5; i++) {
        snprintf(w, sizeof w, "t:warning f:X3RLY%d pos:39.40,-8.20 kind:fire sev:danger ts:%s", i, TS_2026);
        xprsindex_add(st, w, (int)strlen(w), -60, false, 0);
    }
    for (int i = 0; i < 40; i++) {
        snprintf(w, sizeof w, "t:status f:X1A6%02d ts:%s m:here", i, TS_2026);
        xprsindex_add(st, w, (int)strlen(w), -60, false, 0);
    }

    collect_t c = {0};
    xprsidx_query_t q = { .type = XI_T_WARNING, .newest_first = true, .limit = 3 };
    size_t n = xprsindex_query(st, &q, collect, &c);

    CHECK(n == 3, "wanted 3 recent warnings, got %zu", n);
    CHECK(c.n == 3, "callback saw %d", c.n);
    CHECK(c.first.type == XI_T_WARNING, "first is type %d", c.first.type);
    /* Newest first: X3RLY4 was the last warning written. */
    CHECK(strcmp(c.first.from, "X3RLY4") == 0, "newest warning is %s", c.first.from);
    CHECK(strstr(c.first.wire, "kind:fire") != NULL, "wire not kept verbatim");
    xprsindex_close(st);
}

static void test_a_year_ago(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    char w[280];

    for (int i = 0; i < 10; i++) {
        snprintf(w, sizeof w, "t:blog f:X1OLD%d ts:%s m:last year %d", i, TS_2025, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }
    for (int i = 0; i < 10; i++) {
        snprintf(w, sizeof w, "t:blog f:X1NEW%d ts:%s m:this year %d", i, TS_2026, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }

    /* Everything in 2025. */
    collect_t c = {0};
    xprsidx_query_t q = {
        .since_ts = 1735689600u,   /* 2025-01-01 */
        .until_ts = 1767225599u,   /* 2025-12-31 */
        .type = -1, .limit = 100,
    };
    size_t n = xprsindex_query(st, &q, collect, &c);
    CHECK(n == 10, "wanted the 10 from last year, got %zu", n);
    CHECK(strncmp(c.first.from, "X1OLD", 5) == 0, "first is %s", c.first.from);

    /* And the same range narrowed to a type that is not there. */
    collect_t c2 = {0};
    xprsidx_query_t q2 = q;
    q2.type = XI_T_WARNING;
    CHECK(xprsindex_query(st, &q2, collect, &c2) == 0, "warnings appeared from nowhere");
    xprsindex_close(st);
}

/* ── section 36: what may be served ─────────────────────────────────────── */

static void test_mail_is_not_public(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    const char *mail = "t:message f:X1QZ3N d:X1RD89 ts:" TS_2026 " x:pQ4m9xT2vB8kR";
    const char *pub  = "t:warning f:X3RLY7 pos:39.40,-8.20 kind:fire sev:danger ts:" TS_2026;
    CHECK(xprsindex_add(st, mail, (int)strlen(mail), -50, false, 0), "mail not stored");
    CHECK(xprsindex_add(st, pub, (int)strlen(pub), -50, false, 0), "publication not stored");

    /* A stranger sees the publication and never the mail. */
    collect_t c = {0};
    xprsidx_query_t q = { .type = -1, .limit = 50 };
    size_t n = xprsindex_query(st, &q, collect, &c);
    CHECK(n == 1, "a stranger saw %zu records", n);
    CHECK(c.first.type == XI_T_WARNING, "the wrong one survived");

    /* The addressee sees theirs. */
    collect_t c2 = {0};
    xprsidx_query_t q2 = { .type = -1, .limit = 50, .asker = "X1RD89" };
    CHECK(xprsindex_query(st, &q2, collect, &c2) == 2, "addressee cannot read own mail");

    /* So does the sender, and nobody else. */
    collect_t c3 = {0};
    xprsidx_query_t q3 = { .type = -1, .limit = 50, .asker = "X1QZ3N" };
    CHECK(xprsindex_query(st, &q3, collect, &c3) == 2, "sender cannot see what they sent");

    collect_t c4 = {0};
    xprsidx_query_t q4 = { .type = -1, .limit = 50, .asker = "X9NOSY" };
    CHECK(xprsindex_query(st, &q4, collect, &c4) == 1, "a third party read somebody's mail");
    xprsindex_close(st);
}

static void test_refuses_what_it_should(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    const char *ping = "t:ping f:X1A67X ts:" TS_2026;
    const char *pong = "t:pong f:X1A67X ts:" TS_2026;
    CHECK(!xprsindex_add(st, ping, (int)strlen(ping), 0, false, 0), "stored a ping");
    CHECK(!xprsindex_add(st, pong, (int)strlen(pong), 0, false, 0), "stored a pong");

    const char *notxprs = "X1A67X\x1FX1RD89\x1Fhello";
    CHECK(!xprsindex_add(st, notxprs, (int)strlen(notxprs), 0, false, 0),
          "stored a compact frame as XPRS");

    /* The same packet heard twice on two bearers is one record. */
    const char *w = "t:warning f:X3RLY7 kind:fire sev:danger ts:" TS_2026;
    CHECK(xprsindex_add(st, w, (int)strlen(w), -50, false, 0), "first copy refused");
    CHECK(!xprsindex_add(st, w, (int)strlen(w), -80, false, 0), "duplicate stored twice");

    collect_t c = {0};
    xprsidx_query_t q = { .type = -1, .limit = 50 };
    CHECK(xprsindex_query(st, &q, collect, &c) == 1, "store holds the wrong count");
    xprsindex_close(st);
}

/* ── survives a restart, and a truncated index ──────────────────────────── */

static void test_reopen(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    char w[280];
    for (int i = 0; i < 12; i++) {
        snprintf(w, sizeof w, "t:place f:X1PL%02d ts:%s m:spot %d", i, TS_2026, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }
    uint32_t latest = xprsindex_latest_index(st);
    xprsindex_close(st);

    xprsidx_t *re = xprsindex_open(dir);
    CHECK(xprsindex_ready(re), "did not reopen");
    CHECK(xprsindex_latest_index(re) == latest, "index moved across a restart: %u vs %u",
          (unsigned)xprsindex_latest_index(re), (unsigned)latest);

    /* And it keeps counting from where it left off rather than overwriting. */
    const char *more = "t:place f:X1PLNEW ts:" TS_2026 " m:after reboot";
    CHECK(xprsindex_add(re, more, (int)strlen(more), 0, false, 0), "append after reopen failed");
    CHECK(xprsindex_latest_index(re) == latest + 1, "did not continue the sequence");

    /* And every record is READABLE afterwards -- the old ones the scan
     * recovered and the new one written after it. A store that accepts
     * records and then answers nothing is the failure that looks like an
     * empty archive while the count keeps climbing. */
    collect_t c = { 0 };
    xprsidx_query_t q = { .type = -1, .limit = 50, .trusted = true };
    size_t n = xprsindex_query(re, &q, collect, &c);
    CHECK(n == 13, "reopened store served %zu of 13 records", n);
    xprsindex_close(re);
}

static void test_survives_a_lost_index(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    char w[280];
    for (int i = 0; i < 6; i++) {
        snprintf(w, sizeof w, "t:warning f:X3W%02d kind:fire sev:danger ts:%s", i, TS_2026);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }
    xprsindex_close(st);

    /* A power cut between the record and its indexes: the derived files go. */
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -f '%s'/zone.idx '%s'/t/*.idx", dir, dir);
    if (system(cmd) != 0) { /* nothing to remove is fine */ }

    xprsidx_t *re = xprsindex_open(dir);
    collect_t c = {0};
    /* A range query walks segments, so it answers from the records alone. */
    xprsidx_query_t q = { .type = XI_T_WARNING, .limit = 50 };
    size_t n = xprsindex_query(re, &q, collect, &c);
    CHECK(n == 6, "records unreadable without the indexes: got %zu", n);
    xprsindex_close(re);
}

/* ── the shape of the thing on disk ─────────────────────────────────────── */

static void test_wire_is_kept_verbatim(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    /* A packet at the format's limit must survive whole — this is what the
     * 192-byte APRS record could not do, and the reason for a second store. */
    char w[XPRSIDX_WIRE_MAX + 1];
    int n = snprintf(w, sizeof w, "t:blog f:X1LONG ts:%s m:", TS_2026);
    while (n < XPRSIDX_WIRE_MAX) w[n++] = 'x';
    w[XPRSIDX_WIRE_MAX] = '\0';
    CHECK((int)strlen(w) == XPRSIDX_WIRE_MAX, "test packet is %d bytes", (int)strlen(w));
    CHECK(xprsindex_add(st, w, XPRSIDX_WIRE_MAX, 0, false, 0), "250-byte packet refused");

    collect_t c = {0};
    xprsidx_query_t q = { .type = -1, .limit = 5 };
    xprsindex_query(st, &q, collect, &c);
    CHECK(c.n == 1, "got %d records", c.n);
    CHECK(c.first.len == XPRSIDX_WIRE_MAX, "length changed: %u", (unsigned)c.first.len);
    CHECK(strcmp(c.first.wire, w) == 0, "the packet came back different");
    CHECK(c.first.id[0] != '\0', "no identifier derived");
    xprsindex_close(st);
}

/* ── a torn tail must not hide the answers behind it ─────────────────────── */

/*
 * The dongle produced exactly this: the newest entries of t/08.idx were zeroes
 * (FatFs had the file's size but not its bytes), so "the most recent warning"
 * answered nothing while the record sat readable in its segment — and asking
 * for three answered three, because a bigger window reached past the damage.
 */
static void test_torn_tail_still_answers(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    char w[280];

    /* Record 0 is deliberately NOT a warning, as it was on the dongle: that is
     * what let four zero entries quietly answer "no recent warnings". */
    const char *first = "t:observation f:X3WX01 link:ble peers:2 "
                        "ts:2026-08-09_09:00:00";
    xprsindex_add(st, first, (int)strlen(first), -60, false, 0);
    for (int i = 0; i < 6; i++) {
        snprintf(w, sizeof w, "t:warning f:X3RLY%d pos:39.40,-8.20 kind:fire "
                              "sev:danger ts:2026-08-1%d_09:00:00", i, i);
        xprsindex_add(st, w, (int)strlen(w), -60, false, 0);
    }
    /* Real newest warning, the one a reader must get back. */
    const char *newest = "t:warning f:X1LAST pos:38.72,-9.14 kind:flood "
                         "sev:danger ts:2026-08-17_09:00:00";
    CHECK(xprsindex_add(st, newest, (int)strlen(newest), -60, false, 0),
          "newest warning not stored");
    xprsindex_close(st);

    /* Tear the tail: four zero entries appended, as a half-synced file reads. */
    char path[256];
    snprintf(path, sizeof path, "%s/t/%02d.idx", dir, XI_T_WARNING);
    FILE *f = fopen(path, "ab");
    CHECK(f != NULL, "no warning tail to tear");
    if (f) {
        uint32_t zero = 0;
        for (int i = 0; i < 4; i++) fwrite(&zero, sizeof zero, 1, f);
        fclose(f);
    }

    st = xprsindex_open(dir);
    collect_t c = {0};
    xprsidx_query_t q = { .type = XI_T_WARNING, .newest_first = true, .limit = 1 };
    size_t n = xprsindex_query(st, &q, collect, &c);
    CHECK(n == 1, "asked for 1 recent warning past a torn tail, got %zu", n);
    CHECK(strcmp(c.first.from, "X1LAST") == 0,
          "wrong warning came back: %s", c.first.from);

    /* And the same store answers a larger request consistently. */
    collect_t c3 = {0};
    xprsidx_query_t q3 = { .type = XI_T_WARNING, .newest_first = true, .limit = 3 };
    CHECK(xprsindex_query(st, &q3, collect, &c3) == 3, "limit 3 did not fill");
    CHECK(strcmp(c3.first.from, "X1LAST") == 0, "newest-first order broken");
    xprsindex_close(st);
}

/*
 * A hole is not a record, whatever the bytes in it say.
 *
 * A segment slot that was never written holds whatever the cluster held
 * before it was freed. On the bench that was the rotated log file, so a
 * store served records whose from: was a piece of an ESP log line and whose
 * ts: was a log timestamp -- and the JSON they were rendered into carried a
 * raw newline, which emptied the page reading it.
 *
 * Written here the way the card writes it: a real record, then a slot filled
 * with log text, then a real record after the hole. Both real records must
 * come back and the hole must not.
 */
static void test_a_hole_is_not_a_record(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    const char *a = "t:status f:X1AAAA ts:2026-08-17_10:00:00 m:first";
    const char *b = "t:status f:X1CCCC ts:2026-08-17_10:05:00 m:third";
    CHECK(xprsindex_add(st, a, (int)strlen(a), -50, false, 1786000000),
          "first record not stored");
    xprsindex_close(st);

    /* Slot 1: never written, and holding what the card had. */
    char path[256];
    snprintf(path, sizeof path, "%s/seg_%010u.bin", dir, 0u);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL, "no segment to punch");
    if (f) {
        static const char junk[] =
            "xprs: ble -79 dBm 131B t:observation f:X3H3MZ link:ble peers:2\n"
            "I (239679) wifi:state: run -> init (0)\n"
            "W (241788) health: station up: http api+ lan bearer+\n";
        char slot[320];
        memset(slot, 0, sizeof slot);
        memcpy(slot, junk, sizeof junk - 1);
        /* Stale bytes that pass for a written slot: a length in range and an
         * index that is somebody else's. This is the shape that got served --
         * `len != 0` was the whole test, and old log text satisfies it. */
        uint32_t stale_index = 4242;
        uint16_t plausible_len = 131;
        memcpy(slot + 0,  &stale_index,   sizeof stale_index);
        memcpy(slot + 12, &plausible_len, sizeof plausible_len);
        fseek(f, 320L, SEEK_SET);
        fwrite(slot, sizeof slot, 1, f);
        fclose(f);
    }

    /* Reopened, the store keeps its place: the hole is a slot something was
     * written into as far as the file is concerned, so the next record goes
     * after it rather than on top of the records already there. */
    st = xprsindex_open(dir);

    /* And a record written after it is still reachable. */
    CHECK(xprsindex_add(st, b, (int)strlen(b), -50, false, 1786000300),
          "record after the hole not stored");
    collect_t c = {0};
    xprsidx_query_t q = { .type = -1, .limit = 50, .newest_first = false };
    size_t n = xprsindex_query(st, &q, collect, &c);
    CHECK(n == 2, "expected the two real records and not the hole, got %zu", n);
    CHECK(strstr(c.first.from, "xprs") == NULL &&
          strchr(c.first.from, '\n') == NULL,
          "log text was served as a callsign: %s", c.first.from);
    xprsindex_close(st);
}

/* ── authorship (XPRS.md §9.1) ───────────────────────────────────────────── */

/* A verifier the test drives: X1GOOD signs for itself, X1EVIL does not, and
 * nobody has ever heard of X1WHO. */
static int fake_verify(const char *wire, int len, const char *from)
{
    (void)wire; (void)len;
    if (strcmp(from, "X1GOOD") == 0) return 1;
    if (strcmp(from, "X1EVIL") == 0) return -1;
    return 0;
}

static bool collect_flags(const xprsidx_rec_t *rec, void *ctx)
{
    uint8_t *out = (uint8_t *)ctx;
    if (strcmp(rec->from, "X1GOOD") == 0) out[0] = rec->flags;
    if (strcmp(rec->from, "X1EVIL") == 0) out[1] = rec->flags;
    if (strcmp(rec->from, "X1WHO")  == 0) out[2] = rec->flags;
    if (strcmp(rec->from, "X1BARE") == 0) out[3] = rec->flags;
    return true;
}

static void test_verifies_what_it_stores(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(st != NULL, "open failed");
    xprsindex_set_verifier(st, fake_verify);

    CHECK(xprsindex_add(st, "t:status f:X1GOOD ts:2026-08-17_10:00:00 sig:aaa m:hello",
                        56, -40, false, 1786000000), "signed+valid refused");
    CHECK(xprsindex_add(st, "t:status f:X1WHO ts:2026-08-17_10:00:01 sig:bbb m:hello",
                        55, -40, false, 1786000001), "signed+unknown refused");
    CHECK(xprsindex_add(st, "t:status f:X1BARE ts:2026-08-17_10:00:02 m:hello",
                        47, -40, false, 1786000002), "unsigned refused");
    /* Accepted by add() — it is only a lie once the signature has been checked,
     * and that happens a thread later. */
    xprsindex_add(st, "t:status f:X1EVIL ts:2026-08-17_10:00:03 sig:ccc m:hello",
                  56, -40, false, 1786000003);

    uint8_t f[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    xprsidx_query_t q = { .type = -1, .limit = 50, .newest_first = false };
    xprsindex_query(st, &q, collect_flags, f);

    CHECK(f[0] != 0xFF, "the verified record was not stored");
    CHECK(xprsidx_sig_of(f[0]) == XI_SIG_VERIFIED,
          "a checked signature did not read as verified (flags %02x)", f[0]);
    CHECK(f[1] == 0xFF, "a FORGED record was stored — it must be refused");
    CHECK(xprsidx_sig_of(f[2]) == XI_SIG_UNVERIFIED,
          "an author we hold no key for should be unverified, not a verdict");
    CHECK(xprsidx_sig_of(f[3]) == XI_SIG_UNSIGNED,
          "an unsigned packet is not the same as an unverified one");

    xprsidx_stats_t stats;
    xprsindex_stats(st, &stats);
    CHECK(stats.verified == 1 && stats.unverified == 1 && stats.forged == 1,
          "counters wrong: %u/%u/%u", (unsigned)stats.verified,
          (unsigned)stats.unverified, (unsigned)stats.forged);
    CHECK(stats.count == 3, "the forged record still counts (%u)",
          (unsigned)stats.count);

    /* With no verifier a station says the honest thing: it cannot tell. */
    xprsindex_close(st);
    rm_rf(dir);
    st = xprsindex_open(dir);
    xprsindex_add(st, "t:status f:X1GOOD ts:2026-08-17_10:00:00 sig:aaa m:hello",
                  56, -40, false, 1786000000);
    uint8_t g[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    xprsindex_query(st, &q, collect_flags, g);
    CHECK(xprsidx_sig_of(g[0]) == XI_SIG_UNVERIFIED,
          "without a verifier, a signature must not read as verified");
    xprsindex_close(st);
}

/* ── the directory an indexer publishes (XPRS.md §36.9) ──────────────────── */

static void test_directory(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    /* Two stations, one heard twice, plus a piece of mail — whose ADDRESSEE
     * must not appear. A directory says who this indexer archives; naming the
     * recipient would publish the envelope §36.7 keeps. */
    const char *feed[] = {
        "t:warning f:X3RLY7 kind:fire sev:danger ts:2026-08-10_09:00:00",
        "t:info f:X3RLY7 ts:2026-08-14_09:00:00 m:later",
        "t:blog f:X1BOA3 ts:2026-08-12_09:00:00 m:hello",
        "t:message f:X1QZ3N d:X1SECRET ts:2026-08-13_09:00:00 x:pQ4m9",
    };
    for (size_t i = 0; i < sizeof feed / sizeof feed[0]; i++) {
        xprsindex_add(st, feed[i], (int)strlen(feed[i]), -60, false, 0);
    }

    xprsidx_dir_entry_t e[8];
    int n = xprsindex_directory(st, e, 8);
    CHECK(n == 3, "listed %d stations, wanted 3", n);
    if (n == 3) {
        CHECK(strcmp(e[0].call, "X1BOA3") == 0, "not sorted: first is %s", e[0].call);
        CHECK(strcmp(e[1].call, "X1QZ3N") == 0, "second is %s", e[1].call);
        CHECK(strcmp(e[2].call, "X3RLY7") == 0, "third is %s", e[2].call);
        CHECK(e[2].last_ts == xi_expect_ts("2026-08-14_09:00:00"),
              "kept the older timestamp for a station heard twice");
    }
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(e[i].call, "X1SECRET") != 0,
              "a mail addressee was published in the directory");
    }

    char text[512];
    int len = xprsindex_dir_render(e, n, text, sizeof text);
    CHECK(len > 0, "render refused");
    CHECK(strncmp(text, "XDIR1\n", 6) == 0, "no XDIR1 header");
    CHECK(strstr(text, "X3RLY7 2026-08-14_09:00:00") != NULL,
          "line missing or misformatted:\n%s", text);

    /* A directory that does not fit is truncated, never overrun. */
    char tiny[16];
    CHECK(xprsindex_dir_render(e, n, tiny, sizeof tiny) == -1,
          "rendered into a buffer too small for it");
    xprsindex_close(st);
}

/* The directory is CACHED, so what it says must keep matching the store. It is
 * rebuilt by walking every record, which on a filled card is hundreds of
 * thousands of reads with the store's lock held -- the announce path asked for
 * one every ten minutes. Appends fold in instead; this pins that the folding
 * agrees with the walk. */
static void test_directory_tracks_the_store(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    const char *first = "t:info f:X1AAAA ts:2026-08-10_09:00:00 m:one";
    CHECK(xprsindex_add(st, first, (int)strlen(first), -60, false, 0), "first");

    xprsidx_dir_entry_t e[8];
    CHECK(xprsindex_directory(st, e, 8) == 1, "wanted one station");

    /* Now the cache is warm. A station heard for the FIRST time after that
     * has to appear, and one heard again has to move its last_ts forward. */
    const char *second = "t:info f:X1BBBB ts:2026-08-11_09:00:00 m:two";
    const char *again  = "t:info f:X1AAAA ts:2026-08-12_09:00:00 m:three";
    CHECK(xprsindex_add(st, second, (int)strlen(second), -60, false, 0), "second");
    CHECK(xprsindex_add(st, again, (int)strlen(again), -60, false, 0), "again");

    int n = xprsindex_directory(st, e, 8);
    CHECK(n == 2, "cached directory listed %d, wanted 2", n);
    if (n == 2) {
        CHECK(strcmp(e[0].call, "X1AAAA") == 0, "first is %s", e[0].call);
        CHECK(e[0].last_ts == xi_expect_ts("2026-08-12_09:00:00"),
              "a cached entry kept a stale last_ts");
        CHECK(strcmp(e[1].call, "X1BBBB") == 0, "second is %s", e[1].call);
    }

    /* A packet the store REFUSES must not reach the directory: the station
     * publishes this list, and it may only name what it actually holds. */
    const char *ping = "t:ping f:X1NOPE ts:2026-08-13_09:00:00";
    xprsindex_add(st, ping, (int)strlen(ping), -60, false, 0);
    n = xprsindex_directory(st, e, 8);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(e[i].call, "X1NOPE") != 0,
              "a refused packet was published in the directory");
    }

    /* Reopening rebuilds by walking: the walk and the cache must agree. */
    xprsindex_close(st);
    st = xprsindex_open(dir);
    xprsidx_dir_entry_t w[8];
    int m = xprsindex_directory(st, w, 8);
    CHECK(m == 2, "the walk found %d where the cache had 2", m);
    if (m == 2) {
        CHECK(strcmp(w[0].call, "X1AAAA") == 0 &&
              w[0].last_ts == xi_expect_ts("2026-08-12_09:00:00"),
              "the walk disagrees with the cache about X1AAAA");
    }
    xprsindex_close(st);
}

/* Eviction is billed on the bytes the store REALLY holds. It used to bill
 * every segment as full, so a store that had just opened its second segment
 * was charged for 8192 records when it held 4100 -- and threw away 4096 of
 * them, other people's mail included, to get under a budget it was already
 * inside. */
static void test_evicts_on_real_bytes(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);

    char w[300];
    const char *oldest = "t:info f:X1FRST ts:2026-01-01_09:00:00 m:the oldest";
    CHECK(xprsindex_add(st, oldest, (int)strlen(oldest), 0, false, 0), "oldest");
    /* Just over one segment: 4096 records fill segment 0, the rest open
     * segment 1 and leave it nearly empty. */
    for (int i = 0; i < 4100; i++) {
        snprintf(w, sizeof w, "t:info f:X1FL%02d ts:2026-02-01_09:00:00 m:fill %d",
                 i % 90, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }

    xprsidx_stats_t s0;
    xprsindex_stats(st, &s0);
    CHECK(s0.segments == 2, "wanted 2 segments, got %u", (unsigned)s0.segments);

    /* 1.5 MB. The store holds ~4101 records = ~1.31 MB and is INSIDE it;
     * billed by the segment it would read as 2 * 4096 * 320 = 2.5 MB and
     * evict. */
    xprsindex_set_max_bytes(st, 1536u * 1024u);
    snprintf(w, sizeof w, "t:info f:X1LST2 ts:2026-02-02_09:00:00 m:the trigger");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, 0), "trigger add");

    xprsidx_stats_t s1;
    xprsindex_stats(st, &s1);
    CHECK(s1.segments == 2, "evicted while inside the budget (%u segments left)",
          (unsigned)s1.segments);
    xprsidx_query_t q = { .type = -1, .from = "X1FRST", .limit = 5,
                          .trusted = true };
    collect_t c = { 0 };
    CHECK(xprsindex_query(st, &q, collect, &c) == 1,
          "the oldest record was evicted while the store was inside its budget");

    /* Past the budget for real, and it must still evict. */
    xprsindex_set_max_bytes(st, 512u * 1024u);
    snprintf(w, sizeof w, "t:info f:X1LST3 ts:2026-02-03_09:00:00 m:over");
    xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    xprsidx_stats_t s2;
    xprsindex_stats(st, &s2);
    CHECK(s2.segments == 1, "did not evict over budget (%u segments)",
          (unsigned)s2.segments);
    xprsindex_close(st);
}


/*
 * XPRS.md 10.7: a station with no clock stamps epoch:B.S, and a receiver that
 * HAS a clock "records the wall-clock time at which it first heard a given
 * epoch, and can then date every packet of that epoch, including packets
 * delivered days later".
 *
 * Before this, such a record was stored with the arrival time or with nothing,
 * and a record with ts 0 is invisible to every since:/until: window -- so a
 * station that could not say the time could not be caught up with either.
 */
static void test_epoch_is_anchored(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    const uint32_t now = 1787000000u;      /* what OUR clock says */
    char w[300];

    /* X3WX01 has no clock. Its first packet is 3600 s into boot 7, heard now:
     * boot 7 therefore began at now - 3600. */
    snprintf(w, sizeof w,
             "t:observation f:X3WX01 link:ble peers:2 epoch:7.3600");
    CHECK(xprsindex_add(st, w, (int)strlen(w), -60, false, now), "first epoch packet");

    /* A second packet of the same boot, 4210 s in, but CARRIED to us two days
     * later. Its date is the epoch's, not the day it arrived. */
    snprintf(w, sizeof w,
             "t:observation f:X3WX01 link:ble peers:3 epoch:7.4210");
    CHECK(xprsindex_add(st, w, (int)strlen(w), -60, false, now + 172800u),
          "carried epoch packet");

    collect_t c = { 0 };
    xprsidx_query_t q = { .type = -1, .limit = 10, .newest_first = false,
                          .trusted = true };
    CHECK(xprsindex_query(st, &q, collect, &c) == 2, "both packets stored");
    const uint32_t started = now - 3600u;
    CHECK(c.first.ts == started + 3600u, "first packet dated %u, wanted %u",
          (unsigned)c.first.ts, (unsigned)(started + 3600u));
    CHECK(c.last.ts == started + 4210u,
          "a carried packet was dated by its arrival (%u), not its epoch (%u)",
          (unsigned)c.last.ts, (unsigned)(started + 4210u));

    /* And now it can be paged, which is the whole point. */
    collect_t c2 = { 0 };
    xprsidx_query_t q2 = { .type = -1, .limit = 10, .trusted = true,
                           .since_ts = started + 4000u };
    CHECK(xprsindex_query(st, &q2, collect, &c2) == 1,
          "since: over an anchored epoch returned %d", c2.n);

    /* A different boot of the same station is a different epoch. */
    snprintf(w, sizeof w, "t:observation f:X3WX01 link:ble peers:1 epoch:8.10");
    CHECK(xprsindex_add(st, w, (int)strlen(w), -60, false, now + 200000u),
          "next boot stored");
    collect_t c3 = { 0 };
    xprsidx_query_t q3 = { .type = -1, .limit = 10, .trusted = true,
                           .since_ts = now + 190000u };
    CHECK(xprsindex_query(st, &q3, collect, &c3) == 1,
          "the new boot was not dated from its own anchor");
    xprsindex_close(st);
}

/*
 * XPRS.md 13.12.3: `q:mail` asks how much mail a station holds, `only:`
 * names whose. One packet answers a question whose usual answer is nothing,
 * where the alternative -- cmd:history only:X -- replays the mail itself.
 */
static void test_mail_count(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");

    char w[300];
    for (int i = 0; i < 3; i++) {
        snprintf(w, sizeof w,
                 "t:message f:X1QZ3N d:X1BOA3 ts:" TS_2026 " m:waiting %d", i);
        CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, 0), "mail %d", i);
    }
    snprintf(w, sizeof w,
             "t:message f:X1QZ3N d:X1OTHR ts:" TS_2026 " m:for somebody else");
    xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    /* Not mail: no d:, so it is the spool and must not be counted. */
    snprintf(w, sizeof w, "t:status f:X1QZ3N ts:" TS_2026 " m:just talking");
    xprsindex_add(st, w, (int)strlen(w), 0, false, 0);

    CHECK(xprsindex_mail_count(st, "X1BOA3", 99) == 3,
          "wanted 3 for X1BOA3, got %d", xprsindex_mail_count(st, "X1BOA3", 99));
    CHECK(xprsindex_mail_count(st, "X1NONE", 99) == 0,
          "a callsign with no mail was told there is some");
    CHECK(xprsindex_mail_count(st, NULL, 99) == 4,
          "the station's own total is wrong (%d)",
          xprsindex_mail_count(st, NULL, 99));
    /* The cap is a cap: the question is whether there is any. */
    CHECK(xprsindex_mail_count(st, "X1BOA3", 2) == 2, "cap not honoured");
    /* A suffixed device of the same operator is the same recipient (3.1). */
    CHECK(xprsindex_mail_count(st, "X1BOA3-7", 99) == 3,
          "a suffixed callsign did not find its own mail");
    xprsindex_close(st);
}

/*
 * XPRS.md 36.11, in the order it states: the spool goes first, then mail for
 * callsigns that did not name this station, and mail for a callsign whose
 * t:mailbox hold: names it is the last thing an archiver may drop.
 *
 * Being present every day used to earn that last class too. It does not:
 * the section says the classes read off the packet, and a station somebody
 * declared is not outranked by a station that merely talks a lot. X1HERE is
 * here daily and declares nothing; X1DECL declares. Only X1DECL's mail is
 * promised.
 */
static void test_declared_mail_is_the_last_to_go(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");
    xprsindex_set_own(st, "X3ARC1");

    char w[300];
    const uint32_t day0 = 1787000000u;

    /* X1HERE turns up every day and never declares. */
    for (int d = 0; d < 5; d++) {
        snprintf(w, sizeof w,
                 "t:status f:X1HERE ts:" TS_2026 " m:morning %d", d);
        xprsindex_add(st, w, (int)strlen(w), 0, false, day0 + (uint32_t)d * 86400u);
    }
    /* X1DECL names this station as where to leave its mail (13.12). */
    snprintf(w, sizeof w,
             "t:mailbox f:X1DECL ts:" TS_2026 " hold:X3ARC1,X32DVA");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, day0), "declaration");

    /* Mail for each, then fill past the cap so eviction has to choose. */
    snprintf(w, sizeof w,
             "t:message f:X1QZ3N d:X1DECL ts:" TS_2025 " m:for the declarer");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, day0), "declared mail");
    snprintf(w, sizeof w,
             "t:message f:X1QZ3N d:X1HERE ts:" TS_2025 " m:for the regular");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, day0), "undeclared mail");
    for (int i = 0; i < 9000; i++) {
        snprintf(w, sizeof w, "t:info f:X1SP%02d ts:" TS_2026 " m:filler %d",
                 i % 90, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, day0);
    }
    xprsindex_set_max_bytes(st, 1u * 1024u * 1024u);
    snprintf(w, sizeof w, "t:info f:X1LAST ts:" TS_2026 " m:the drop");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, day0), "post-cap add");

    xprsidx_query_t q = { .type = -1, .from = "X1QZ3N", .asker = "X1DECL",
                          .limit = 5 };
    collect_t c = { 0 };
    CHECK(xprsindex_query(st, &q, collect, &c) >= 1,
          "the declarer's mail was evicted -- 36.11 keeps it longest");

    /* Eviction really ran, so the choice above was a choice: the 9000 filler
     * records filled three segments and at least one was retired. (`count`
     * is cumulative -- records ever accepted -- so segments is what says a
     * segment went.) */
    xprsidx_stats_t s;
    xprsindex_stats(st, &s);
    CHECK(s.segments < 3, "nothing was evicted (%u segments)",
          (unsigned)s.segments);
    xprsindex_close(st);
}

/*
 * `only:` is a CALLSIGN (36.6) and `kind:` is a TYPE (25.2). Reading only: as
 * a type made only:message appear to work while the spec's own only:X5A3F2
 * matched nothing at all.
 */
static void test_only_is_a_callsign(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");
    xprsindex_set_own(st, "X3ARC1");

    const char *a = "t:message f:X1AAAA ts:" TS_2026 " m:from aaaa";
    const char *b = "t:message f:X1BBBB ts:" TS_2026 " m:from bbbb";
    const char *o = "t:observation f:X1AAAA ts:" TS_2026 " link:lan peers:1";
    xprsindex_add(st, a, (int)strlen(a), 0, false, 0);
    xprsindex_add(st, b, (int)strlen(b), 0, false, 0);
    xprsindex_add(st, o, (int)strlen(o), 0, false, 0);

    /* only: picks the station, whatever the type. */
    xprsidx_query_t q = { .type = -1, .only = "X1AAAA", .limit = 10,
                          .trusted = true };
    collect_t c = { 0 };
    CHECK(xprsindex_query(st, &q, collect, &c) == 2,
          "only: did not match a callsign across types");

    /* kind: picks the type, whatever the station. */
    xprsidx_query_t q2 = { .type = xprsidx_type_code("message"), .limit = 10,
                           .trusted = true };
    collect_t c2 = { 0 };
    CHECK(xprsindex_query(st, &q2, collect, &c2) == 2,
          "kind: did not match a type across stations");

    /* Together they intersect. */
    xprsidx_query_t q3 = { .type = xprsidx_type_code("message"),
                           .only = "X1AAAA", .limit = 10, .trusted = true };
    collect_t c3 = { 0 };
    CHECK(xprsindex_query(st, &q3, collect, &c3) == 1,
          "only: and kind: did not intersect");

    /* With neither, a replay serves the talking and not the beacons. */
    xprsidx_query_t q4 = { .type = -1, .talk_only = true, .limit = 10,
                           .trusted = true };
    collect_t c4 = { 0 };
    CHECK(xprsindex_query(st, &q4, collect, &c4) == 2,
          "an unfiltered replay handed back presence chatter");
    xprsindex_close(st);
}

/* XPRS.md 25.2: kind: may be a comma-separated list, and 36.9.3's
 * neighbourhood ask depends on it. The failure this pins: the responder used
 * to truncate the list into a 16-byte buffer, xprsidx_type_code mapped the
 * fragment to OTHER, and the replay served nothing -- silently, for exactly
 * the ask the spec standardises. */
static void test_kind_takes_a_list(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");
    xprsindex_set_own(st, "X3ARC1");

    const char *m = "t:message f:X1AAAA ts:" TS_2026 " m:town channel";
    const char *i = "t:info f:X1BBBB pos:38.72,-9.14 kind:rain ts:" TS_2026;
    const char *w = "t:warning f:X3RLY7 kind:fire sev:danger ts:" TS_2026;
    const char *o = "t:observation f:X1AAAA ts:" TS_2026 " link:lan peers:1";
    xprsindex_add(st, m, (int)strlen(m), 0, false, 0);
    xprsindex_add(st, i, (int)strlen(i), 0, false, 0);
    xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    xprsindex_add(st, o, (int)strlen(o), 0, false, 0);

    /* The mask parser itself. */
    CHECK(xprsidx_type_mask(NULL) == 0, "NULL did not map to no-mask");
    CHECK(xprsidx_type_mask("") == 0, "empty did not map to no-mask");
    CHECK(xprsidx_type_mask("message") == (1u << XI_T_MESSAGE),
          "single name did not map to its bit");
    CHECK(xprsidx_type_mask("info,warning") ==
              ((1u << XI_T_INFO) | (1u << XI_T_WARNING)),
          "list did not OR its bits");
    CHECK(xprsidx_type_mask("nonsense") == (1u << XI_T_OTHER),
          "unknown name did not map to OTHER, as storage does");

    /* The 36.9.3 ask, minus the kinds this store happens not to hold. */
    xprsidx_query_t q = {
        .type = -1,
        .types = xprsidx_type_mask("info,warning,event,status,blog,message"),
        .limit = 10, .trusted = true,
    };
    collect_t c = { 0 };
    CHECK(xprsindex_query(st, &q, collect, &c) == 3,
          "the list ask did not match every listed type");

    /* A one-entry list behaves exactly like the single form. */
    xprsidx_query_t q1 = { .type = -1, .types = xprsidx_type_mask("message"),
                           .limit = 10, .trusted = true };
    collect_t c1 = { 0 };
    CHECK(xprsindex_query(st, &q1, collect, &c1) == 1,
          "a one-entry list diverged from the single form");

    /* The mask REPLACES talk_only: the asker said what it wants, and what it
     * wants here includes a presence type an unfiltered replay would drop. */
    xprsidx_query_t q2 = { .type = -1,
                           .types = xprsidx_type_mask("observation"),
                           .talk_only = true, .limit = 10, .trusted = true };
    collect_t c2 = { 0 };
    CHECK(xprsindex_query(st, &q2, collect, &c2) == 1,
          "a listed presence type was suppressed by talk_only");
    xprsindex_close(st);
}

static void test_retention_priorities(const char *dir)
{
    rm_rf(dir);
    xprsidx_t *st = xprsindex_open(dir);
    CHECK(xprsindex_ready(st), "store did not open");
    xprsindex_set_own(st, "X3ARC1");

    /* X1FAV declares this station its mailbox (class 3 for its mail). */
    const char *decl =
        "t:mailbox f:X1FAV ts:" TS_2026 " hold:X3ARC1,X3OTHER";
    CHECK(xprsindex_add(st, decl, (int)strlen(decl), 0, false, 0),
          "declaration not stored");

    /* Mail for the declared callsign, mail for a stranger, and spool. */
    char w[300];
    snprintf(w, sizeof w,
             "t:message f:X1QZ3N d:X1FAV ts:" TS_2025 " m:for the favourite");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, 0), "fav mail");
    snprintf(w, sizeof w,
             "t:message f:X1QZ3N d:X1WHO ts:" TS_2025 " m:custody mail");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, 0), "custody mail");
    for (int i = 0; i < 9000; i++) {   /* fill past two segments of spool */
        snprintf(w, sizeof w,
                 "t:info f:X1SP%02d ts:" TS_2026 " m:spool filler %d",
                 i % 90, i);
        xprsindex_add(st, w, (int)strlen(w), 0, false, 0);
    }

    /* Cap below the store's size: eviction must fire on the next add. */
    xprsindex_set_max_bytes(st, 1u * 1024u * 1024u);   /* ~3276 records */
    snprintf(w, sizeof w, "t:info f:X1LAST ts:" TS_2026 " m:the drop");
    CHECK(xprsindex_add(st, w, (int)strlen(w), 0, false, 0), "post-cap add");

    /*
     * 36.11 is an ORDER, not a set. Class 3 -- mail for a station that chose
     * this one -- must survive; class 2, somebody else's mail carried as a
     * favour, is what pays when the store is still over budget after the
     * segment goes.
     *
     * The old version of this test asserted only that BOTH survived, which is
     * why nobody noticed that xi_evict_locked computed the class and threw the
     * answer away: classes 2 and 3 were the same thing and the test could not
     * tell.
     */
    xprsidx_query_t q = { .type = -1, .from = "X1QZ3N", .asker = "X1FAV",
                          .limit = 5 };
    collect_t c = { 0 };
    CHECK(xprsindex_query(st, &q, collect, &c) >= 1,
          "declared mail was evicted");
    xprsidx_query_t q2 = { .type = -1, .from = "X1QZ3N", .asker = "X1WHO",
                           .limit = 5 };
    collect_t c2 = { 0 };
    size_t custody = xprsindex_query(st, &q2, collect, &c2);
    xprsidx_stats_t pressure;
    xprsindex_stats(st, &pressure);
    const uint64_t after = (uint64_t)pressure.segments * 4096u * 320u;
    if (after > 1u * 1024u * 1024u) {
        CHECK(custody == 0,
              "custody mail outlived the budget it should have paid");
    } else {
        CHECK(custody >= 1, "custody mail dropped while there was room");
    }
    xprsidx_query_t q3 = { .type = -1, .from = "X1SP00", .limit = 500,
                           .trusted = true };
    collect_t c3 = { 0 };
    size_t remaining = xprsindex_query(st, &q3, collect, &c3);
    xprsidx_stats_t xs;
    xprsindex_stats(st, &xs);
    CHECK((uint64_t)xs.segments * 4096u * 320u <= 1u * 1024u * 1024u + 4096u * 320u,
          "store did not shrink under its cap (%u segments)",
          (unsigned)xs.segments);
    (void)remaining;

    /* The newest-ts the store reports is the catch-up since: (36.10). */
    CHECK(xprsindex_newest_ts(st) >= xprsindex_ts_to_epoch(TS_2026,
          (int)strlen(TS_2026)), "newest ts wrong");
    xprsindex_close(st);
}
int main(void)
{
    const char *dir = "/tmp/xprsidx_test";
    /* Line-buffered: piped into a log or a test runner, stdout is otherwise
     * fully buffered and NOTHING appears until the process exits. A slow run
     * then looks like a hung one, which is exactly how this was misread. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("xprsindex host tests\n");
    test_recent_of_a_type(dir);
    test_a_year_ago(dir);
    test_mail_is_not_public(dir);
    test_refuses_what_it_should(dir);
    test_reopen(dir);
    test_survives_a_lost_index(dir);
    test_wire_is_kept_verbatim(dir);
    test_torn_tail_still_answers(dir);
    test_a_hole_is_not_a_record(dir);
    test_directory(dir);
    test_directory_tracks_the_store(dir);
    test_evicts_on_real_bytes(dir);
    test_verifies_what_it_stores(dir);
    test_retention_priorities(dir);
    test_declared_mail_is_the_last_to_go(dir);
    test_mail_count(dir);
    test_epoch_is_anchored(dir);
    test_only_is_a_callsign(dir);
    test_kind_takes_a_list(dir);
    rm_rf(dir);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
