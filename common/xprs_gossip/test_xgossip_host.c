/*
 * Host test for the gossip store. The point of these is the WALLS: gossip is
 * believed by the routing, so the interesting cases are all the ones where a
 * claim must NOT be believed.
 */
#include "xgossip.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static int checks, failed;
#define CHECK(cond, ...) do {                                              \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failed++;                                                      \
            printf("  FAIL %s:%d  ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static void rm_rf(const char *dir)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char p[512];
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
    mkdir(dir, 0777);
}

static const char *H1[] = { "X1AAAA" };
static const char *H2[] = { "X1AAAA", "X1BBBB" };

/* An unsigned claim about who is where costs nothing to send, so it must buy
 * nothing. Our own radio is the one witness that needs no signature. */
static void test_signer_credited(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);          /* need-to-know out of the way */

    xgossip_note_hears(g, "X3OBS1", H1, 1, "ble", false, 1000);
    xgossip_pump(g);
    xgossip_sighting_t s[8];
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) == 0,
          "an unsigned hears: claim was believed");

    xgossip_note_hears(g, "X3OBS1", H1, 1, "ble", true, 1000);
    xgossip_pump(g);
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) > 0,
          "a signed hears: claim was not recorded");

    xgossip_stats_t st;
    xgossip_stats(g, &st);
    CHECK(st.refused_unsigned == 1, "refused_unsigned is %u",
          (unsigned)st.refused_unsigned);
    xgossip_close(g);
}

/* One observer's gossip at the rate its own adverts arrive: a neighbour that
 * beacons every two seconds must not own the table. */
static void test_signer_quota(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);

    CHECK(xgossip_would_accept(g, "X3OBS1", 1000), "first ask refused");
    xgossip_note_hears(g, "X3OBS1", H1, 1, "ble", true, 1000);
    CHECK(!xgossip_would_accept(g, "X3OBS1", 1002),
          "the pre-check would have paid for a verify the quota then refuses");
    xgossip_note_hears(g, "X3OBS1", H2, 2, "ble", true, 1002);
    xgossip_pump(g);

    xgossip_sighting_t s[8];
    CHECK(xgossip_where_is(g, "X1BBBB", s, 8) == 0,
          "a second observation inside the quota window was taken");

    /* A DIFFERENT observer is metered on its own clock, not the first one's. */
    CHECK(xgossip_would_accept(g, "X3OBS2", 1002), "observers share a meter");
    xgossip_note_hears(g, "X3OBS2", H2, 2, "ble", true, 1002);
    xgossip_pump(g);
    CHECK(xgossip_where_is(g, "X1BBBB", s, 8) > 0,
          "a second observer was refused on the first one's meter");

    /* Past the window, the first observer is heard again. */
    xgossip_note_hears(g, "X3OBS1", H2, 2, "ble", true,
                       1000 + XGOSSIP_SIGNER_SEC + 1);
    xgossip_pump(g);
    xgossip_stats_t st;
    xgossip_stats(g, &st);
    CHECK(st.refused_quota == 1, "refused_quota is %u",
          (unsigned)st.refused_quota);
    xgossip_close(g);
}

/*
 * Radio truth (36.9.4). "X was heard via a hub" says nothing about where X is,
 * so the internet bearer may light the live layer and must never write the
 * durable one -- otherwise a station that once crossed the internet is
 * remembered forever as being next to whoever relayed it.
 */
static void test_radio_truth(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);

    xgossip_note_hears(g, "X3HUB1", H1, 1, "rns", true, 1000);
    xgossip_pump(g);
    xgossip_sighting_t s[8];
    int n = xgossip_where_is(g, "X1AAAA", s, 8);
    CHECK(n == 1, "an rns sighting gave %d rows", n);
    if (n >= 1) CHECK(s[0].layer == 3, "rns wrote layer %d", s[0].layer);

    xgossip_note_hears(g, "X3RAD1", H1, 1, "espnow", true, 2000);
    xgossip_pump(g);
    n = xgossip_where_is(g, "X1AAAA", s, 8);
    int visits = 0;
    for (int i = 0; i < n; i++) if (s[i].layer == 2) visits++;
    CHECK(visits == 1, "espnow left %d visit rows, wanted 1", visits);
    for (int i = 0; i < n; i++)
        if (s[i].layer == 2)
            CHECK(strcmp(s[i].gw, "X3RAD1") == 0,
                  "the durable layer names %s", s[i].gw);
    xgossip_close(g);
}

/* Our own radio is its own witness -- and beacons arrive several a second, so
 * the same hearing must not be paid for twice. */
static void test_direct(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);

    xgossip_note_direct(g, "X1AAAA", "X3SELF", "ble", 1000);
    xgossip_note_direct(g, "X1AAAA", "X3SELF", "ble", 1001);
    xgossip_note_direct(g, "X1AAAA", "X3SELF", "ble", 1002);
    xgossip_pump(g);
    xgossip_stats_t st;
    xgossip_stats(g, &st);
    CHECK(st.accepted == 1, "%u hearings of one beacon were paid for",
          (unsigned)st.accepted);

    xgossip_sighting_t s[8];
    const int n = xgossip_where_is(g, "X1AAAA", s, 8);
    CHECK(n == 2, "own hearing gave %d rows, wanted a live and a visit", n);

    /* Ourselves is not a sighting of anybody. */
    xgossip_note_direct(g, "X3SELF", "X3SELF", "ble", 2000);
    xgossip_pump(g);
    CHECK(xgossip_where_is(g, "X3SELF", s, 8) == 0, "the station sighted itself");
    xgossip_close(g);
}

/* A live sighting outranks a place somebody used to be, and inside a layer the
 * freshest gateway is the one worth trying first. */
static void test_ranking_and_try(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);

    xgossip_note_hears(g, "X3OLD1", H1, 1, "espnow", true, 1000);
    xgossip_pump(g);
    xgossip_note_hears(g, "X3NEW1", H1, 1, "espnow", true, 5000);
    xgossip_pump(g);
    xgossip_note_hears(g, "X3HUB1", H1, 1, "rns", true, 9000);
    xgossip_pump(g);

    xgossip_sighting_t s[8];
    const int n = xgossip_where_is(g, "X1AAAA", s, 8);
    CHECK(n >= 3, "only %d sightings", n);
    CHECK(s[0].layer == 3, "the freshest answer is layer %d", s[0].layer);
    CHECK(strcmp(s[0].gw, "X3HUB1") == 0, "freshest first gave %s", s[0].gw);
    int first_visit = -1;
    for (int i = 0; i < n; i++) if (s[i].layer == 2) { first_visit = i; break; }
    CHECK(first_visit > 0, "a visit outranked a live sighting");

    char list[64];
    const int w = xgossip_try_candidates(g, "X1AAAA", "X3NEW1", list, sizeof list);
    CHECK(w > 0, "m:try had nothing to offer");
    CHECK(strstr(list, "X3NEW1") == NULL,
          "m:try named the station that just answered 404: %s", list);
    CHECK(strncmp(list, "X3HUB1", 6) == 0, "m:try leads with %s", list);
    xgossip_close(g);
}

/*
 * Need-to-know (36.9.4). An ordinary station keeps gossip in proportion to its
 * duties; a super keeps every callsign it can learn of, because being the
 * station that remembers what the others could not IS the role.
 */
static void test_need_to_know(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);

    /* A stranger, heard about from somebody else, is not this station's
     * business. */
    xgossip_note_hears(g, "X3OBS1", H1, 1, "espnow", true, 1000);
    xgossip_pump(g);
    xgossip_sighting_t s[8];
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) == 0,
          "an ordinary station kept gossip about a callsign it never met");

    /* One it has heard itself is. And once it is known, what others say
     * about it is worth keeping too. */
    xgossip_note_direct(g, "X1AAAA", "X3SELF", "ble", 2000);
    xgossip_pump(g);
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) > 0, "own hearing was refused");
    xgossip_note_hears(g, "X3OBS1", H1, 1, "espnow", true, 3000);
    xgossip_pump(g);
    int n = xgossip_where_is(g, "X1AAAA", s, 8), from_obs = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].gw, "X3OBS1") == 0) from_obs++;
    CHECK(from_obs > 0, "gossip about a known callsign was still refused");

    /* The same station, promoted. */
    xgossip_set_super(g, true);
    xgossip_note_hears(g, "X3OBS2", H2, 2, "espnow", true, 4000);
    xgossip_pump(g);
    CHECK(xgossip_where_is(g, "X1BBBB", s, 8) > 0,
          "a super refused a callsign it had not met");
    xgossip_close(g);
}

/* Gossip that does not survive a reboot is gossip a station has to relearn
 * from an air it may not hear again. */
static void test_survives_a_reboot(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);
    xgossip_note_hears(g, "X3OBS1", H2, 2, "espnow", true, 1000);
    xgossip_pump(g);
    xgossip_close(g);

    g = xgossip_open(dir);
    xgossip_set_super(g, true);
    xgossip_sighting_t s[8];
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) > 0, "X1AAAA lost across a reopen");
    CHECK(xgossip_where_is(g, "X1BBBB", s, 8) > 0, "X1BBBB lost across a reopen");
    xgossip_stats_t st;
    xgossip_stats(g, &st);
    CHECK(st.rows == 4, "reopened with %u rows, wanted 4", (unsigned)st.rows);
    xgossip_close(g);
}

/* The per-callsign cap: many gateways for one station, stalest dropped. */
static void test_per_callsign_cap(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);

    for (int i = 0; i < XGOSSIP_LIVE_G + 4; i++) {
        char gw[10];
        snprintf(gw, sizeof gw, "X3G%03d", i);
        /* rns: live only, so the cap under test is G and not K. */
        xgossip_note_hears(g, gw, H1, 1, "rns", true,
                           (uint32_t)(1000 + i * (XGOSSIP_SIGNER_SEC + 1)));
        xgossip_pump(g);
    }
    xgossip_sighting_t s[32];
    const int n = xgossip_where_is(g, "X1AAAA", s, 32);
    CHECK(n == XGOSSIP_LIVE_G, "kept %d live sightings, cap is %d",
          n, XGOSSIP_LIVE_G);
    /* The ones kept are the freshest, so the earliest gateway is gone. */
    for (int i = 0; i < n; i++)
        CHECK(strcmp(s[i].gw, "X3G000") != 0, "the stalest gateway survived");
    xgossip_close(g);
}

/* An SSID is not a different station (3.1). */
static void test_ssid_is_the_same_station(const char *dir)
{
    rm_rf(dir);
    xgossip_t *g = xgossip_open(dir);
    xgossip_set_super(g, true);
    const char *h[] = { "X1AAAA-7" };
    xgossip_note_hears(g, "X3OBS1", h, 1, "espnow", true, 1000);
    xgossip_pump(g);
    xgossip_sighting_t s[8];
    CHECK(xgossip_where_is(g, "X1AAAA", s, 8) > 0,
          "X1AAAA-7 was filed as a station of its own");
    xgossip_close(g);
}

int main(void)
{
    const char *dir = "/tmp/xgossip_test";
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("xgossip host tests\n");
    test_signer_credited(dir);
    test_signer_quota(dir);
    test_radio_truth(dir);
    test_direct(dir);
    test_ranking_and_try(dir);
    test_need_to_know(dir);
    test_survives_a_reboot(dir);
    test_per_callsign_cap(dir);
    test_ssid_is_the_same_station(dir);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
